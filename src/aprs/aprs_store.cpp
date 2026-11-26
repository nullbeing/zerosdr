#include "aprs/aprs_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace aprs {
namespace {

constexpr double kEarthRadiusNm = 3440.065;
constexpr double kPi = 3.14159265358979323846;

// APRS traffic is sparse compared with AIS: keep stations visible for half
// an hour after the last packet and drop them after two hours.
constexpr int kStaleSeconds = 30 * 60;
constexpr int kRemoveSeconds = 2 * 60 * 60;

// Track history: enough for a long drive, with a distance threshold that
// ignores GPS jitter while a station is parked.
constexpr size_t kTrailMaxPoints = 120;
constexpr int kTrailMaxSeconds = 2 * 60 * 60;
constexpr double kTrailMinMoveDegrees = 0.00012;  // about 13 m

std::string trim(std::string value) {
    while (!value.empty() && value.back() == ' ') value.pop_back();
    size_t start = 0;
    while (start < value.size() && value[start] == ' ') ++start;
    value.erase(0, start);
    return value;
}

char kindFor(const Packet& packet) {
    if (packet.weather) return 'W';
    if (packet.message) return 'M';
    switch (packet.type) {
        case '>': return 'S';
        case '_': return 'W';
        case ';':
        case ')': return 'O';
        case 'T': return 'T';
        case ':': return 'M';
        default: break;
    }
    return packet.has_position ? 'P' : 'S';
}

}  // namespace

void Store::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    stations_.clear();
    log_.clear();
    total_packets_ = 0;
}

void Store::setStation(double latitude, double longitude, bool valid) {
    std::lock_guard<std::mutex> lock(mutex_);
    station_lat_ = latitude;
    station_lon_ = longitude;
    station_valid_ = valid;
    for (auto& entry : stations_) updateDistanceLocked(entry.second);
}

bool Store::hasStation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_valid_;
}

double Store::stationLatitude() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_lat_;
}

double Store::stationLongitude() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_lon_;
}

void Store::apply(const Packet& packet) {
    if (packet.source.empty()) return;
    const time_t now = time(nullptr);

    std::lock_guard<std::mutex> lock(mutex_);

    StationState& station = stations_[packet.source];
    if (station.callsign.empty()) {
        station.callsign = packet.source;
        station.first_seen = now;
    }
    ++total_packets_;
    ++station.packets;
    station.last_seen = now;
    station.packet_type = packet.type;
    station.kind = kindFor(packet);

    if (!packet.destination.empty()) station.destination = packet.destination;
    if (!packet.path.empty()) {
        station.path.clear();
        for (size_t i = 0; i < packet.path.size(); ++i) {
            if (i > 0) station.path += ',';
            station.path += packet.path[i];
        }
    }
    if (!packet.info.empty()) station.last_info = packet.info;
    if (!packet.comment.empty()) station.comment = trim(packet.comment);

    if (packet.has_position) {
        const bool first = !station.has_position;
        const double dlat = packet.latitude - station.latitude;
        const double dlon =
            (packet.longitude - station.longitude) *
            std::cos(packet.latitude * kPi / 180.0);
        const bool moved =
            first || std::hypot(dlat, dlon) >= kTrailMinMoveDegrees;
        station.has_position = true;
        station.latitude = packet.latitude;
        station.longitude = packet.longitude;
        station.symbol_table = packet.symbol_table;
        station.symbol = packet.symbol;

        if (moved) {
            TrailPoint point;
            point.time = now;
            point.latitude = packet.latitude;
            point.longitude = packet.longitude;
            point.speed_knots = packet.speed_knots >= 0.0f
                                    ? packet.speed_knots
                                    : station.speed_knots;
            point.course = packet.course >= 0.0f ? packet.course
                                                  : station.course;
            point.altitude_ft = packet.altitude_ft > -100000
                                    ? packet.altitude_ft
                                    : station.altitude_ft;
            station.trail.push_back(point);
            while (station.trail.size() > kTrailMaxPoints) {
                station.trail.pop_front();
            }
            while (station.trail.size() > 1 &&
                   now - station.trail.front().time > kTrailMaxSeconds) {
                station.trail.pop_front();
            }
        }
    }
    if (packet.speed_knots >= 0.0f) station.speed_knots = packet.speed_knots;
    if (packet.course >= 0.0f) station.course = packet.course;
    if (packet.altitude_ft > -100000) station.altitude_ft = packet.altitude_ft;

    if (packet.weather) {
        station.weather = true;
        if (packet.wind_dir >= 0.0f) station.wind_dir = packet.wind_dir;
        if (packet.wind_speed_mph >= 0.0f) {
            station.wind_speed_mph = packet.wind_speed_mph;
        }
        station.temperature_c = packet.temperature_c;
        if (packet.rain_hour_mm >= 0.0f) {
            station.rain_hour_mm = packet.rain_hour_mm;
        }
    }

    if (station.has_position) {
        updateDistanceLocked(station);
    } else {
        station.distance_nm = -1.0f;
    }

    LogEntry entry;
    entry.time = now;
    entry.source = packet.source;
    entry.kind = station.kind;

    char prefix[32];
    struct tm local {};
    localtime_r(&now, &local);
    std::snprintf(prefix, sizeof(prefix), "%02d:%02d:%02d %-9s %c",
                  local.tm_hour, local.tm_min, local.tm_sec,
                  packet.source.c_str(), entry.kind);

    std::string detail = packet.info;
    if (detail.size() > 96) detail.resize(96);
    entry.text = std::string(prefix) + " " + detail;
    log_.push_back(std::move(entry));
    while (log_.size() > 300) log_.pop_front();

    pruneLocked(now);
}

void Store::pruneLocked(time_t now) {
    for (auto it = stations_.begin(); it != stations_.end();) {
        const time_t age = now - it->second.last_seen;
        it->second.stale = age > kStaleSeconds;
        if (age > kRemoveSeconds) {
            it = stations_.erase(it);
        } else {
            ++it;
        }
    }
}

void Store::updateDistanceLocked(StationState& station) const {
    if (!station_valid_ || !station.has_position) {
        station.distance_nm = -1.0f;
        return;
    }
    station.distance_nm = distanceNm(station_lat_, station_lon_,
                                     station.latitude, station.longitude);
    station.bearing_deg = bearingDeg(station_lat_, station_lon_,
                                     station.latitude, station.longitude);
}

float Store::distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double dlat = (lat2 - lat1) * kPi / 180.0;
    const double dlon = (lon2 - lon1) * kPi / 180.0;
    const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                     std::cos(lat1 * kPi / 180.0) *
                         std::cos(lat2 * kPi / 180.0) *
                         std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    return static_cast<float>(2.0 * kEarthRadiusNm *
                              std::asin(std::min(1.0, std::sqrt(a))));
}

float Store::bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = lat1 * kPi / 180.0;
    const double phi2 = lat2 * kPi / 180.0;
    const double dlon = (lon2 - lon1) * kPi / 180.0;
    const double y = std::sin(dlon) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2) -
                     std::sin(phi1) * std::cos(phi2) * std::cos(dlon);
    double degrees = std::atan2(y, x) * 180.0 / kPi;
    if (degrees < 0.0) degrees += 360.0;
    return static_cast<float>(degrees);
}

std::vector<StationState> Store::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(time(nullptr));

    std::vector<StationState> result;
    result.reserve(stations_.size());
    for (const auto& entry : stations_) result.push_back(entry.second);

    std::sort(result.begin(), result.end(),
              [](const StationState& a, const StationState& b) {
                  if (a.distance_nm >= 0.0f && b.distance_nm >= 0.0f) {
                      return a.distance_nm < b.distance_nm;
                  }
                  if (a.distance_nm >= 0.0f) return true;
                  if (b.distance_nm >= 0.0f) return false;
                  if (a.last_seen != b.last_seen) {
                      return a.last_seen > b.last_seen;
                  }
                  return a.callsign < b.callsign;
              });
    return result;
}

std::vector<LogEntry> Store::logSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<LogEntry>(log_.begin(), log_.end());
}

uint64_t Store::totalPackets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_packets_;
}

size_t Store::stationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stations_.size();
}

const char* Store::kindLabel(char kind) {
    switch (kind) {
        case 'P': return "POSITION";
        case 'W': return "WEATHER";
        case 'S': return "STATUS";
        case 'M': return "MESSAGE";
        case 'O': return "OBJECT";
        case 'T': return "TELEMETRY";
        default: return "PACKET";
    }
}

}  // namespace aprs
