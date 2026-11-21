#include "ais/ais_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ais {
namespace {

constexpr double kEarthRadiusNm = 3440.065;
constexpr double kPi = 3.14159265358979323846;

constexpr int kStaleSeconds = 60;
constexpr int kRemoveSeconds = 600;

std::string trim(std::string value) {
    while (!value.empty() && value.back() == ' ') value.pop_back();
    size_t start = 0;
    while (start < value.size() && value[start] == ' ') ++start;
    value.erase(0, start);
    return value;
}

}  // namespace

void AisStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    vessels_.clear();
    log_.clear();
    total_messages_ = 0;
}

void AisStore::setStation(double latitude, double longitude, bool valid) {
    std::lock_guard<std::mutex> lock(mutex_);
    station_lat_ = latitude;
    station_lon_ = longitude;
    station_valid_ = valid;
    for (auto& entry : vessels_) updateDistanceLocked(entry.second);
}

bool AisStore::hasStation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_valid_;
}

double AisStore::stationLatitude() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_lat_;
}

double AisStore::stationLongitude() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_lon_;
}

void AisStore::apply(const AisFrame& frame) {
    AisMessage message;
    if (!parseAisMessage(frame, message) || !message.valid) return;

    const time_t now = time(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);

    VesselState& vessel = vessels_[message.mmsi];
    if (vessel.mmsi == 0) vessel.mmsi = message.mmsi;

    ++total_messages_;
    ++vessel.messages;
    vessel.last_seen = now;
    vessel.last_type = message.type;

    if (!message.name.empty()) vessel.name = trim(message.name);
    if (!message.callsign.empty()) vessel.callsign = trim(message.callsign);
    if (!message.destination.empty()) vessel.destination = trim(message.destination);
    if (message.ship_type >= 0) vessel.ship_type = message.ship_type;
    if (message.imo != 0) vessel.imo = message.imo;
    if (message.epfd >= 0) vessel.epfd = message.epfd;
    if (message.to_bow >= 0) vessel.to_bow = message.to_bow;
    if (message.to_stern >= 0) vessel.to_stern = message.to_stern;
    if (message.to_port >= 0) vessel.to_port = message.to_port;
    if (message.to_starboard >= 0) vessel.to_starboard = message.to_starboard;
    if (message.draught > 0.0f) vessel.draught = message.draught;
    if (message.eta_month != 0 || message.eta_day != 0 ||
        message.eta_hour != 0 || message.eta_minute != 0) {
        vessel.eta_month = message.eta_month;
        vessel.eta_day = message.eta_day;
        vessel.eta_hour = message.eta_hour;
        vessel.eta_minute = message.eta_minute;
    }
    if (message.class_b) vessel.class_b = true;
    if (message.is_aton) vessel.aton = true;
    if (message.is_base_station) vessel.base_station = true;
    if (!message.name.empty() || !message.callsign.empty() ||
        message.ship_type >= 0 || message.imo != 0 ||
        message.destination.size() > 0) {
        vessel.last_static = now;
    }

    if (message.has_position) {
        const bool moved = !vessel.has_position ||
                           std::fabs(vessel.latitude - message.latitude) > 1e-5 ||
                           std::fabs(vessel.longitude - message.longitude) > 1e-5;
        vessel.has_position = true;
        vessel.latitude = message.latitude;
        vessel.longitude = message.longitude;
        if (moved) {
            vessel.trail.emplace_back(message.latitude, message.longitude);
            while (vessel.trail.size() > 10) vessel.trail.pop_front();
        }
    }
    if (message.sog >= 0.0f) vessel.sog = message.sog;
    if (message.cog >= 0.0f) vessel.cog = message.cog;
    if (message.heading >= 0) vessel.heading = message.heading;
    if (message.nav_status >= 0) vessel.nav_status = message.nav_status;

    if (vessel.has_position) {
        updateDistanceLocked(vessel);
        updateCpaLocked(vessel);
    } else {
        vessel.distance_nm = -1.0f;
        vessel.cpa_nm = -1.0f;
        vessel.tcpa_min = -1.0f;
    }

    AisLogEntry entry;
    entry.time = now;
    entry.channel = static_cast<char>('A' + frame.channel);
    entry.type = message.type;
    entry.mmsi = message.mmsi;

    char line[128];
    struct tm local {};
    localtime_r(&now, &local);
    int length = std::snprintf(
        line, sizeof(line), "%02d:%02d:%02d %c t%02d %09u",
        local.tm_hour, local.tm_min, local.tm_sec, entry.channel,
        message.type, message.mmsi);
    if (length < 0) length = 0;
    int used = std::min<int>(length, static_cast<int>(sizeof(line)) - 1);

    if (!vessel.name.empty() && used < static_cast<int>(sizeof(line)) - 1) {
        used += std::snprintf(line + used, sizeof(line) - used, " \"%s\"",
                              vessel.name.c_str());
    } else if (message.has_position &&
               used < static_cast<int>(sizeof(line)) - 1) {
        used += std::snprintf(line + used, sizeof(line) - used, " %.4f,%.4f",
                              message.latitude, message.longitude);
    }
    if (message.sog >= 0.0f && used < static_cast<int>(sizeof(line)) - 1) {
        std::snprintf(line + used, sizeof(line) - used, " %.1fkn",
                      message.sog);
    }
    entry.text = line;
    log_.push_back(std::move(entry));
    while (log_.size() > 200) log_.pop_front();

    pruneLocked(now);
}

void AisStore::pruneLocked(time_t now) {
    for (auto it = vessels_.begin(); it != vessels_.end();) {
        const time_t age = now - it->second.last_seen;
        it->second.stale = age > kStaleSeconds;
        if (age > kRemoveSeconds) {
            it = vessels_.erase(it);
        } else {
            ++it;
        }
    }
}

void AisStore::updateDistanceLocked(VesselState& vessel) const {
    if (!station_valid_ || !vessel.has_position) {
        vessel.distance_nm = -1.0f;
        return;
    }
    vessel.distance_nm = distanceNm(station_lat_, station_lon_,
                                    vessel.latitude, vessel.longitude);
    vessel.bearing_deg = bearingDeg(station_lat_, station_lon_,
                                    vessel.latitude, vessel.longitude);
}

void AisStore::updateCpaLocked(VesselState& vessel) const {
    if (!station_valid_ || !vessel.has_position) {
        vessel.cpa_nm = -1.0f;
        vessel.tcpa_min = -1.0f;
        return;
    }

    const double lat0 = station_lat_ * kPi / 180.0;
    const double north_nm = (vessel.latitude - station_lat_) * 60.0;
    const double east_nm =
        (vessel.longitude - station_lon_) * 60.0 * std::cos(lat0);

    double vn = 0.0;
    double ve = 0.0;
    if (vessel.sog > 0.5f && vessel.cog >= 0.0f) {
        const double course = vessel.cog * kPi / 180.0;
        ve = vessel.sog * std::sin(course);
        vn = vessel.sog * std::cos(course);
    }

    const double speed2 = ve * ve + vn * vn;
    double tcpa_hours = 0.0;
    bool closing = false;
    if (speed2 > 1e-6) {
        tcpa_hours = -(east_nm * ve + north_nm * vn) / speed2;
        closing = (tcpa_hours >= 0.0);
        if (!closing) tcpa_hours = 0.0;
    }

    const double cpa_east = east_nm + ve * tcpa_hours;
    const double cpa_north = north_nm + vn * tcpa_hours;
    vessel.cpa_east_nm = static_cast<float>(cpa_east);
    vessel.cpa_north_nm = static_cast<float>(cpa_north);
    vessel.cpa_nm = static_cast<float>(std::hypot(cpa_east, cpa_north));
    vessel.tcpa_min = (speed2 > 0.25 && closing)
                          ? static_cast<float>(tcpa_hours * 60.0)
                          : -1.0f;
}

float AisStore::distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double dlat = (lat2 - lat1) * kPi / 180.0;
    const double dlon = (lon2 - lon1) * kPi / 180.0;
    const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                     std::cos(lat1 * kPi / 180.0) *
                         std::cos(lat2 * kPi / 180.0) *
                         std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    return static_cast<float>(2.0 * kEarthRadiusNm *
                              std::asin(std::min(1.0, std::sqrt(a))));
}

float AisStore::bearingDeg(double lat1, double lon1, double lat2, double lon2) {
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

std::vector<VesselState> AisStore::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(time(nullptr));

    std::vector<VesselState> result;
    result.reserve(vessels_.size());
    for (const auto& entry : vessels_) result.push_back(entry.second);

    std::sort(result.begin(), result.end(),
              [](const VesselState& a, const VesselState& b) {
                  if (a.distance_nm >= 0.0f && b.distance_nm >= 0.0f) {
                      return a.distance_nm < b.distance_nm;
                  }
                  if (a.distance_nm >= 0.0f) return true;
                  if (b.distance_nm >= 0.0f) return false;
                  if (a.last_seen != b.last_seen) return a.last_seen > b.last_seen;
                  return a.mmsi < b.mmsi;
              });
    return result;
}

std::vector<AisLogEntry> AisStore::logSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<AisLogEntry>(log_.begin(), log_.end());
}

uint64_t AisStore::totalMessages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_messages_;
}

size_t AisStore::vesselCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vessels_.size();
}

const char* AisStore::shipTypeLabel(int ship_type) {
    if (ship_type >= 70 && ship_type < 80) return "CARGO";
    if (ship_type >= 80 && ship_type < 90) return "TANKER";
    if (ship_type >= 60 && ship_type < 70) return "PASS";
    if (ship_type == 30) return "FISH";
    if (ship_type == 31 || ship_type == 32 || ship_type == 52) return "TUG";
    if (ship_type == 36 || ship_type == 37) return "SAIL";
    if (ship_type == 50) return "PILOT";
    if (ship_type == 51) return "SAR";
    if (ship_type == 55) return "LAW";
    return "";
}

}  // namespace ais
