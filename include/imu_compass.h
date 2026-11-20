#ifndef IMU_COMPASS_H
#define IMU_COMPASS_H

#include <string>

/**
 * ImuCompass - tilt-compensated compass for the Cardputer Zero
 *
 * Some Cardputer Zero boards carry a BMI270 accelerometer plus a BMM150
 * magnetometer (the magnetometer hangs off the BMI270 auxiliary I2C bus).
 * The Linux kernel already binds both chips through the industrial I/O
 * framework, so the values are read from /sys/bus/iio/devices - no raw I2C
 * access and no register poking is needed here.
 *
 * The heading is the direction the top edge of the device points, in
 * degrees clockwise from magnetic north (0 = north, 90 = east). When the
 * board has no IMU, open() simply returns false and the app keeps working.
 *
 * The heading is smoothed as a unit vector, so it never jumps when the
 * angle crosses 0/360. A configurable offset can rotate the result to match
 * how the chip is mounted inside the case.
 */
class ImuCompass {
public:
    ImuCompass() = default;

    /**
     * Find the accelerometer and magnetometer IIO devices.
     * @return true when both are present and readable
     */
    bool open();

    void close();

    bool available() const { return available_; }

    /** Human-readable description, e.g. "bmi270 + bmm150". */
    const std::string& deviceName() const { return device_name_; }

    /**
     * Poll the sensors (internally rate-limited to ~20 Hz) and update the
     * smoothed heading. Safe to call every frame.
     */
    void update(float delta_time);

    bool hasHeading() const { return has_heading_; }

    /** Smoothed heading in degrees (0..360, clockwise from north). */
    float heading() const { return heading_; }

    /** Unsmoothed heading for the most recent sample (0..360). */
    float rawHeading() const { return raw_heading_; }

    /** Magnetic field magnitude of the last sample, in gauss. */
    float fieldStrength() const { return field_strength_; }

    void setOffsetDeg(float deg) { offset_deg_ = deg; }
    float offsetDeg() const { return offset_deg_; }

    /** Flip the rotation direction (for mirrored chip mounts). */
    void setInvert(bool invert) { invert_ = invert; }
    bool invert() const { return invert_; }

    /** Drop the smoothing history (after changing the offset). */
    void resetSmoothing() { ema_valid_ = false; }

private:
    static bool readFloat(const std::string& path, float& value);
    static bool readVector(const std::string& dir, const char* prefix,
                           float scale, float& x, float& y, float& z);
    static bool findDevice(const std::string& attribute, std::string& dir,
                           std::string& name);

    std::string accel_dir_;
    std::string magn_dir_;
    std::string device_name_ = "not found";
    bool available_ = false;
    bool has_heading_ = false;

    float accel_scale_ = 0.0f;
    float magn_scale_ = 0.0f;

    // BMI270 gyroscope. The magnetometer sits next to the speaker magnet on
    // this board; its raw field is dominated by a fixed offset, so the gyro
    // carries short-term yaw and the magnetometer is only a slow correction
    // when its field strength looks like Earth's.
    bool gyro_available_ = false;
    float gyro_scale_ = 0.0f;
    float gyro_bias_ = 0.0f;
    float gyro_bias_sum_ = 0.0f;
    int gyro_bias_samples_ = 0;
    bool gyro_bias_valid_ = false;
    float gyro_heading_ = 0.0f;
    bool gyro_heading_valid_ = false;
    float mag_heading_ = 0.0f;

    float heading_ = 0.0f;
    float raw_heading_ = 0.0f;
    float field_strength_ = 0.0f;

    // Unit-vector exponential smoothing.
    float ema_sin_ = 0.0f;
    float ema_cos_ = 0.0f;
    bool ema_valid_ = false;

    float sample_timer_ = 0.0f;
    int read_failures_ = 0;

    float offset_deg_ = 0.0f;
    bool invert_ = false;
};

#endif  // IMU_COMPASS_H
