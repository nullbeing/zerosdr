#ifndef GPS_LOCATOR_H
#define GPS_LOCATOR_H

#include <chrono>
#include <string>

/**
 * GpsLocator - best-effort position source for the ADSB page.
 *
 * Supports two back-ends, in order:
 *   1. gpsd on 127.0.0.1:2947 (JSON TPV reports)
 *   2. raw NMEA on common serial devices (9600/38400/115200)
 *
 * poll() is non-blocking and is meant to be called from the UI loop.
 * The device has no GPS (the common case), in which case hasFix() stays
 * false and statusText() reports "NOT FOUND".
 */
class GpsLocator {
public:
    GpsLocator() = default;
    ~GpsLocator();

    GpsLocator(const GpsLocator&) = delete;
    GpsLocator& operator=(const GpsLocator&) = delete;

    /** Read whatever is available and update the fix state. */
    void poll();

    bool hasFix() const;
    double latitude() const { return latitude_; }
    double longitude() const { return longitude_; }

    /** "gpsd", "serial" or "none". */
    const char* sourceName() const;
    /** "FIX", "SEARCHING", "NOT FOUND". */
    const char* statusText() const;

private:
    using Clock = std::chrono::steady_clock;

    void pollGpsd();
    void pollSerial();
    void tryOpenNextSerial();
    void parseNmeaLine(const char* line);
    void parseGpsdLine(const std::string& line);
    void setFix(double lat, double lon, const char* source);

    // Fix state
    double latitude_ = 0.0;
    double longitude_ = 0.0;
    bool valid_ = false;
    Clock::time_point fix_time_{};
    std::string source_ = "none";

    // gpsd
    int gpsd_fd_ = -1;
    std::string gpsd_buf_;
    Clock::time_point next_gpsd_try_{};

    // serial
    int serial_fd_ = -1;
    std::string serial_buf_;
    std::string serial_path_;
    bool serial_seen_data_ = false;
    size_t serial_device_index_ = 0;
    size_t serial_baud_index_ = 0;
    Clock::time_point serial_opened_at_{};
    Clock::time_point next_serial_try_{};
};

#endif // GPS_LOCATOR_H
