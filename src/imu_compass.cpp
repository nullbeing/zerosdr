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
    device_name_ = accel_name + " + " + magn_name;
    available_ = true;
    has_heading_ = false;
    ema_valid_ = false;
    read_failures_ = 0;
    sample_timer_ = kSamplePeriod;  // take the first sample next update()

    std::cout << "ImuCompass: using " << device_name_ << " ("
              << accel_dir_ << ", " << magn_dir_ << ")" << std::endl;
    return true;
}

void ImuCompass::close() {
    accel_dir_.clear();
    magn_dir_.clear();
    available_ = false;
    has_heading_ = false;
    ema_valid_ = false;
    read_failures_ = 0;
}

void ImuCompass::update(float delta_time) {
    if (!available_) return;
    if (delta_time <= 0.0f) return;

    sample_timer_ += delta_time;
    if (sample_timer_ < kSamplePeriod) return;
    sample_timer_ = 0.0f;

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    if (!readVector(accel_dir_, "in_accel", accel_scale_, ax, ay, az) ||
        !readVector(magn_dir_, "in_magn", magn_scale_, mx, my, mz)) {
        if (++read_failures_ > 40) {
            std::cerr << "ImuCompass: lost " << device_name_ << std::endl;
            available_ = false;
            has_heading_ = false;
        }
        return;
    }
    read_failures_ = 0;

    // Accelerometer in g (the driver reports m/s^2).
    ax /= kGravity;
    ay /= kGravity;
    az /= kGravity;

    const float accel_norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (accel_norm < 0.6f || accel_norm > 1.4f) {
        // Not a usable gravity reference (free fall or heavy shaking).
        return;
    }

    field_strength_ =
        std::sqrt(mx * mx + my * my + mz * mz);
    if (field_strength_ < 0.05f) {
        // No usable field; keep the last heading if there is one.
        return;
    }

    // Roll and pitch from the gravity vector.
    const float roll = std::atan2(ay, az);
    const float pitch = std::atan2(-ax, std::sqrt(ay * ay + az * az));

    // Rotate the magnetometer reading into the horizontal plane.
    const float sin_roll = std::sin(roll);
    const float cos_roll = std::cos(roll);
    const float sin_pitch = std::sin(pitch);
    const float cos_pitch = std::cos(pitch);

    const float mag_x =
        mx * cos_pitch + my * sin_roll * sin_pitch + mz * cos_roll * sin_pitch;
    const float mag_y = my * cos_roll - mz * sin_roll;

    float degrees = std::atan2(-mag_y, mag_x) * kRadToDeg;
    if (invert_) degrees = -degrees;
    degrees += offset_deg_;
    degrees = std::fmod(degrees, 360.0f);
    if (degrees < 0.0f) degrees += 360.0f;

    raw_heading_ = degrees;

    // Smooth sin/cos separately so 359° -> 1° does not sweep the long way.
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
    heading_ = smoothed;
    has_heading_ = true;
}
