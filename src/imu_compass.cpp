#include <algorithm>
#include "imu_compass.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

constexpr float kGravity = 9.80665f;      // m/s^2 per g
constexpr float kRadToDeg = 57.2957795f;
constexpr float kSamplePeriod = 0.05f;    // 20 Hz
constexpr float kSmoothing = 0.25f;       // per-sample unit-vector EMA

}  // namespace

bool ImuCompass::readFloat(const std::string& path, float& value) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    float parsed = 0.0f;
    if (!(in >> parsed)) return false;
    value = parsed;
    return true;
}

bool ImuCompass::readVector(const std::string& dir, const char* prefix,
                            float scale, float& x, float& y, float& z) {
    float raw_x = 0.0f, raw_y = 0.0f, raw_z = 0.0f;
    if (!readFloat(dir + "/" + prefix + "_x_raw", raw_x) ||
        !readFloat(dir + "/" + prefix + "_y_raw", raw_y) ||
        !readFloat(dir + "/" + prefix + "_z_raw", raw_z)) {
        return false;
    }
    x = raw_x * scale;
    y = raw_y * scale;
    z = raw_z * scale;
    return true;
}

bool ImuCompass::findDevice(const std::string& attribute, std::string& dir,
                            std::string& name) {
    namespace fs = std::filesystem;
    const fs::path root("/sys/bus/iio/devices");
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        if (!fs::exists(entry.path() / attribute, ec)) continue;

        std::ifstream in(entry.path() / "name");
        std::string device;
        if (!(in >> device)) device = entry.path().filename().string();
        dir = entry.path().string();
        name = device;
        return true;
    }
    return false;
}

bool ImuCompass::open() {
    close();

    std::string accel_name;
    std::string magn_name;
    if (!findDevice("in_accel_x_raw", accel_dir_, accel_name) ||
        !findDevice("in_magn_x_raw", magn_dir_, magn_name)) {
        std::string missing;
        if (accel_dir_.empty()) missing = "accelerometer";
        else missing = "magnetometer";
        device_name_ = "no " + missing;
        std::cout << "ImuCompass: IMU not available (" << device_name_
                  << ")" << std::endl;
        return false;
    }

    float accel_scale = 0.0f;
    float magn_scale = 0.0f;
    if (!readFloat(accel_dir_ + "/in_accel_scale", accel_scale) ||
        !readFloat(magn_dir_ + "/in_magn_scale", magn_scale) ||
        accel_scale <= 0.0f || magn_scale <= 0.0f) {
        device_name_ = "scale unreadable";
        std::cout << "ImuCompass: cannot read sensor scales" << std::endl;
        accel_dir_.clear();
        magn_dir_.clear();
        return false;
    }

    // Fail early when the devices are present but not readable (e.g. a
    // permission change), so the radar falls back to north-up cleanly.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!readVector(accel_dir_, "in_accel", accel_scale, x, y, z)) {
        device_name_ = "accelerometer unreadable";
        std::cout << "ImuCompass: cannot read " << accel_dir_ << std::endl;
        accel_dir_.clear();
        magn_dir_.clear();
        return false;
    }

    accel_scale_ = accel_scale;
    magn_scale_ = magn_scale;

    // The gyroscope lives in the same BMI270 IIO device as the accelerometer.
    gyro_available_ = false;
    gyro_scale_ = 0.0f;
    if (readFloat(accel_dir_ + "/in_anglvel_scale", gyro_scale_) &&
        gyro_scale_ > 0.0f) {
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        if (readVector(accel_dir_, "in_anglvel", gyro_scale_, gx, gy, gz)) {
            gyro_available_ = true;
        }
    }

    device_name_ = accel_name + " + " + magn_name;
    available_ = true;
    has_heading_ = false;
    ema_valid_ = false;
    read_failures_ = 0;
    sample_timer_ = kSamplePeriod;  // take the first mag sample next update()

    gyro_bias_ = 0.0f;
    gyro_bias_sum_ = 0.0f;
    gyro_bias_samples_ = 0;
    gyro_bias_valid_ = false;
    gyro_heading_ = 0.0f;
    gyro_heading_valid_ = false;
    mag_heading_ = 0.0f;

    std::cout << "ImuCompass: using " << device_name_ << " ("
              << accel_dir_ << ", " << magn_dir_ << ")"
              << (gyro_available_ ? ", gyro" : ", no gyro") << std::endl;
    return true;
}

void ImuCompass::close() {
    accel_dir_.clear();
    magn_dir_.clear();
    available_ = false;
    has_heading_ = false;
    ema_valid_ = false;
    read_failures_ = 0;
    gyro_available_ = false;
    gyro_bias_valid_ = false;
    gyro_heading_valid_ = false;
}

void ImuCompass::update(float delta_time) {
    if (!available_ || delta_time <= 0.0f) return;

    auto fail_read = [this]() {
        if (++read_failures_ > 40) {
            std::cerr << "ImuCompass: lost " << device_name_ << std::endl;
            available_ = false;
            has_heading_ = false;
        }
    };

    // Gravity vector, every frame. The gyro yaw is the component around this
    // axis, so the compass keeps working when the board is held tilted.
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (!readVector(accel_dir_, "in_accel", accel_scale_, ax, ay, az)) {
        fail_read();
        return;
    }
    ax /= kGravity;
    ay /= kGravity;
    az /= kGravity;
    const float accel_norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (accel_norm < 0.6f || accel_norm > 1.4f) {
        return;  // free fall or heavy shaking
    }
    const float axn = ax / accel_norm;
    const float ayn = ay / accel_norm;
    const float azn = az / accel_norm;

    // The BMM150 sits next to the speaker magnet: its raw field is several
    // gauss and barely rotates with the board. The BMI270 gyro gives a clean
    // short-term yaw, and the magnetometer only corrects it when the field
    // magnitude looks like Earth's (~0.2-1.2 gauss).
    if (gyro_available_) {
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        if (readVector(accel_dir_, "in_anglvel", gyro_scale_,
                       gx, gy, gz)) {
            const float projected = gx * axn + gy * ayn + gz * azn;

            if (!gyro_bias_valid_) {
                gyro_bias_sum_ += projected;
                if (++gyro_bias_samples_ >= 10) {
                    gyro_bias_ = gyro_bias_sum_ / gyro_bias_samples_;
                    gyro_bias_valid_ = true;
                }
            } else if (std::fabs(projected) < 0.06f) {
                // Slow bias tracking only while nearly stationary.
                gyro_bias_ += 0.002f * (projected - gyro_bias_);
            }

            if (gyro_bias_valid_) {
                float rate = -(projected - gyro_bias_);
                if (invert_) rate = -rate;
                const float dt = std::min(delta_time, 0.10f);
                gyro_heading_ += rate * dt * kRadToDeg;
                gyro_heading_ = std::fmod(gyro_heading_, 360.0f);
                if (gyro_heading_ < 0.0f) gyro_heading_ += 360.0f;
                gyro_heading_valid_ = true;
            }
        } else {
            gyro_available_ = false;
        }
    }

    if (gyro_heading_valid_) {
        heading_ = gyro_heading_ + offset_deg_;
        heading_ = std::fmod(heading_, 360.0f);
        if (heading_ < 0.0f) heading_ += 360.0f;
        raw_heading_ = gyro_heading_;
        has_heading_ = true;
    }

    // The magnetometer is only needed at 20 Hz.
    sample_timer_ += delta_time;
    if (sample_timer_ < kSamplePeriod) return;
    sample_timer_ = 0.0f;

    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    if (!readVector(magn_dir_, "in_magn", magn_scale_, mx, my, mz)) {
        fail_read();
        return;
    }
    read_failures_ = 0;

    field_strength_ = std::sqrt(mx * mx + my * my + mz * mz);
    if (field_strength_ < 0.05f) {
        return;  // no usable field; keep the last heading
    }

    const float roll = std::atan2(ay, az);
    const float pitch =
        std::atan2(-ax, std::sqrt(ay * ay + az * az));
    const float sin_roll = std::sin(roll);
    const float cos_roll = std::cos(roll);
    const float sin_pitch = std::sin(pitch);
    const float cos_pitch = std::cos(pitch);

    const float mag_x =
        mx * cos_pitch + my * sin_roll * sin_pitch + mz * cos_roll * sin_pitch;
    const float mag_y = my * cos_roll - mz * sin_roll;

    float degrees = std::atan2(-mag_y, mag_x) * kRadToDeg;
    if (invert_) degrees = -degrees;
    degrees = std::fmod(degrees, 360.0f);
    if (degrees < 0.0f) degrees += 360.0f;
    mag_heading_ = degrees;

    const bool mag_trusted =
        field_strength_ >= 0.20f && field_strength_ <= 1.2f;

    if (gyro_available_) {
        if (!gyro_heading_valid_) {
            // Seed the relative heading from the first field sample so the
            // display starts at a sensible bearing instead of 0. The 0 key
            // aligns it to true north.
            gyro_heading_ = degrees;
            gyro_heading_valid_ = true;
        } else if (mag_trusted) {
            float diff = degrees - gyro_heading_;
            while (diff > 180.0f) diff -= 360.0f;
            while (diff < -180.0f) diff += 360.0f;
            gyro_heading_ += diff * 0.02f;
            gyro_heading_ = std::fmod(gyro_heading_, 360.0f);
            if (gyro_heading_ < 0.0f) gyro_heading_ += 360.0f;
        }
        heading_ = gyro_heading_ + offset_deg_;
        heading_ = std::fmod(heading_, 360.0f);
        if (heading_ < 0.0f) heading_ += 360.0f;
        raw_heading_ = gyro_heading_;
        has_heading_ = true;
        return;
    }

    // Magnetometer-only fallback (boards without a gyro, or gyro failure).
    raw_heading_ = degrees + offset_deg_;
    raw_heading_ = std::fmod(raw_heading_, 360.0f);
    if (raw_heading_ < 0.0f) raw_heading_ += 360.0f;

    const float rad = degrees / kRadToDeg;
    const float s = std::sin(rad);
    const float c = std::cos(rad);
    if (!ema_valid_) {
        ema_sin_ = s;
        ema_cos_ = c;
        ema_valid_ = true;
    } else {
        ema_sin_ += kSmoothing * (s - ema_sin_);
        ema_cos_ += kSmoothing * (c - ema_cos_);
    }

    float smoothed = std::atan2(ema_sin_, ema_cos_) * kRadToDeg;
    if (smoothed < 0.0f) smoothed += 360.0f;
    heading_ = smoothed + offset_deg_;
    heading_ = std::fmod(heading_, 360.0f);
    if (heading_ < 0.0f) heading_ += 360.0f;
    has_heading_ = true;
}
