#include "core/gps_locator.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr int kGpsdPort = 2947;
const char* kSerialDevices[] = {
    "/dev/serial0", "/dev/ttyAMA0", "/dev/ttyS0",
    "/dev/ttyUSB0", "/dev/ttyACM0",
};
const speed_t kBauds[] = {B9600, B38400, B115200};
constexpr int kFixMaxAgeSec = 10;
constexpr int kGpsdRetrySec = 15;
constexpr int kSerialRetrySec = 30;

std::vector<std::string> splitComma(const char* line, size_t max_tokens) {
    std::vector<std::string> out;
    const char* p = line;
    while (*p && out.size() < max_tokens) {
        const char* comma = std::strchr(p, ',');
        if (comma == nullptr) {
            out.emplace_back(p);
            break;
        }
        out.emplace_back(p, static_cast<size_t>(comma - p));
        p = comma + 1;
    }
    return out;
}

// NMEA ddmm.mmmm / dddmm.mmmm -> decimal degrees.
bool parseNmeaCoord(const std::string& value, const std::string& hemi, double& out) {
    if (value.empty() || hemi.empty()) return false;
    char* end = nullptr;
    const double raw = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || end == nullptr) return false;

    const int degrees = static_cast<int>(raw / 100.0);
    const double minutes = raw - degrees * 100.0;
    double result = degrees + minutes / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') result = -result;
    out = result;
    return true;
}

bool extractJsonNumber(const std::string& line, const char* key, double& out) {
    const std::string needle = std::string("\"") + key + "\":";
    size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    char* end = nullptr;
    const double value = std::strtod(line.c_str() + pos, &end);
    if (end == line.c_str() + pos) return false;
    out = value;
    return true;
}
}  // namespace

GpsLocator::~GpsLocator() {
    if (gpsd_fd_ >= 0) close(gpsd_fd_);
    if (serial_fd_ >= 0) close(serial_fd_);
}

bool GpsLocator::hasFix() const {
    if (!valid_) return false;
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                         Clock::now() - fix_time_)
                         .count();
    return age <= kFixMaxAgeSec;
}

const char* GpsLocator::sourceName() const {
    return hasFix() ? source_.c_str() : "none";
}

const char* GpsLocator::statusText() const {
    if (hasFix()) return "FIX";
    if (gpsd_fd_ >= 0 || (serial_fd_ >= 0 && serial_seen_data_)) {
        return "SEARCHING";
    }
    return "NOT FOUND";
}

void GpsLocator::setFix(double lat, double lon, const char* source) {
    latitude_ = lat;
    longitude_ = lon;
    valid_ = true;
    source_ = source;
    fix_time_ = Clock::now();
}

void GpsLocator::poll() {
    pollGpsd();
    pollSerial();
}

void GpsLocator::pollGpsd() {
    const auto now = Clock::now();
    if (gpsd_fd_ < 0) {
        if (now < next_gpsd_try_) return;
        next_gpsd_try_ = now + std::chrono::seconds(kGpsdRetrySec);

        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kGpsdPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 &&
            errno != EINPROGRESS) {
            close(fd);
            return;
        }
        gpsd_fd_ = fd;
        gpsd_buf_.clear();
        const char watch[] = "?WATCH={\"enable\":true,\"json\":true}\n";
        (void)!write(gpsd_fd_, watch, sizeof(watch) - 1);
        return;
    }

    char buf[512];
    for (;;) {
        const ssize_t n = read(gpsd_fd_, buf, sizeof(buf));
        if (n > 0) {
            gpsd_buf_.append(buf, static_cast<size_t>(n));
            if (gpsd_buf_.size() > 8192) gpsd_buf_.erase(0, 4096);
            continue;
        }
        if (n == 0) {
            close(gpsd_fd_);
            gpsd_fd_ = -1;
        }
        break;
    }

    size_t newline;
    while ((newline = gpsd_buf_.find('\n')) != std::string::npos) {
        const std::string line = gpsd_buf_.substr(0, newline);
        gpsd_buf_.erase(0, newline + 1);
        parseGpsdLine(line);
    }
}

void GpsLocator::parseGpsdLine(const std::string& line) {
    if (line.find("\"class\":\"TPV\"") == std::string::npos) return;
    double mode = 0.0;
    if (!extractJsonNumber(line, "mode", mode) || mode < 2.0) return;
    double lat = 0.0;
    double lon = 0.0;
    if (!extractJsonNumber(line, "lat", lat)) return;
    if (!extractJsonNumber(line, "lon", lon)) return;
    setFix(lat, lon, "gpsd");
}

void GpsLocator::pollSerial() {
    const auto now = Clock::now();
    if (serial_fd_ < 0) {
        if (now < next_serial_try_) return;
        tryOpenNextSerial();
        return;
    }

    char buf[256];
    bool got_data = false;
    for (;;) {
        const ssize_t n = read(serial_fd_, buf, sizeof(buf));
        if (n > 0) {
            got_data = true;
            serial_seen_data_ = true;
            serial_buf_.append(buf, static_cast<size_t>(n));
            if (serial_buf_.size() > 4096) serial_buf_.erase(0, 2048);
            continue;
        }
        break;
    }

    size_t newline;
    while ((newline = serial_buf_.find('\n')) != std::string::npos) {
        std::string line = serial_buf_.substr(0, newline);
        serial_buf_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        parseNmeaLine(line.c_str());
    }

    // No data for a while: move on to the next baud rate / device.
    if (!got_data &&
        now - serial_opened_at_ > std::chrono::seconds(4)) {
        close(serial_fd_);
        serial_fd_ = -1;
        ++serial_baud_index_;
        if (serial_baud_index_ >= sizeof(kBauds) / sizeof(kBauds[0])) {
            serial_baud_index_ = 0;
            serial_device_index_ =
                (serial_device_index_ + 1) %
                (sizeof(kSerialDevices) / sizeof(kSerialDevices[0]));
        }
        next_serial_try_ = now;
    }
}

void GpsLocator::tryOpenNextSerial() {
    const char* path = kSerialDevices[serial_device_index_];
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        serial_device_index_ =
            (serial_device_index_ + 1) %
            (sizeof(kSerialDevices) / sizeof(kSerialDevices[0]));
        next_serial_try_ = Clock::now() + std::chrono::seconds(1);
        return;
    }

    termios tio{};
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetispeed(&tio, kBauds[serial_baud_index_]);
        cfsetospeed(&tio, kBauds[serial_baud_index_]);
        tio.c_cflag |= CLOCAL | CREAD;
        (void)tcsetattr(fd, TCSANOW, &tio);
    }

    serial_fd_ = fd;
    serial_path_ = path;
    serial_buf_.clear();
    serial_opened_at_ = Clock::now();
    next_serial_try_ = Clock::now() + std::chrono::seconds(kSerialRetrySec);
}

void GpsLocator::parseNmeaLine(const char* line) {
    if (line == nullptr || line[0] != '$') return;

    const std::vector<std::string> f = splitComma(line, 16);
    if (f.size() < 7) return;

    // $xxRMC: ,time,status,lat,N/S,lon,E/W,...
    if (f[0].size() >= 6 && f[0].compare(f[0].size() - 3, 3, "RMC") == 0) {
        if (f[2] != "A") return;
        double lat = 0.0;
        double lon = 0.0;
        if (!parseNmeaCoord(f[3], f[4], lat)) return;
        if (!parseNmeaCoord(f[5], f[6], lon)) return;
        setFix(lat, lon, "serial");
        return;
    }

    // $xxGGA: ,time,lat,N/S,lon,E/W,quality,...
    if (f[0].size() >= 6 && f[0].compare(f[0].size() - 3, 3, "GGA") == 0) {
        if (f[6].empty() || f[6] == "0") return;
        double lat = 0.0;
        double lon = 0.0;
        if (!parseNmeaCoord(f[2], f[3], lat)) return;
        if (!parseNmeaCoord(f[4], f[5], lon)) return;
        setFix(lat, lon, "serial");
    }
}
