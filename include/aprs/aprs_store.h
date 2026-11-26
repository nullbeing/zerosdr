#ifndef APRS_STORE_H
#define APRS_STORE_H

#include "aprs/aprs_packet.h"

#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aprs {

struct TrailPoint {
    time_t time = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    float speed_knots = -1.0f;
    float course = -1.0f;
    int altitude_ft = -100000;
};

struct StationState {
    std::string callsign;
    std::string destination;
    std::string path;
    std::string comment;
    std::string last_info;

    char packet_type = 0;
    char kind = 'P';  // P position, W weather, S status, M message, O object
    char symbol_table = '/';
    char symbol = '>';

    bool has_position = false;
    double latitude = 0.0;
    double longitude = 0.0;
    float speed_knots = -1.0f;
    float course = -1.0f;
    int altitude_ft = -100000;

    float distance_nm = -1.0f;
    float bearing_deg = 0.0f;

    bool weather = false;
    float wind_dir = -1.0f;
    float wind_speed_mph = -1.0f;
    float temperature_c = 0.0f;
    float rain_hour_mm = -1.0f;

    time_t first_seen = 0;
    time_t last_seen = 0;
    uint32_t packets = 0;
    bool stale = false;

    std::deque<TrailPoint> trail;  // oldest first
};

struct LogEntry {
    time_t time = 0;
    std::string source;
    char kind = 'P';
    std::string text;
};

class Store {
public:
    void clear();
    void apply(const Packet& packet);

    void setStation(double latitude, double longitude, bool valid);
    bool hasStation() const;
    double stationLatitude() const;
    double stationLongitude() const;

    std::vector<StationState> snapshot();
    std::vector<LogEntry> logSnapshot() const;

    uint64_t totalPackets() const;
    size_t stationCount() const;

    static const char* kindLabel(char kind);

private:
    void pruneLocked(time_t now);
    void updateDistanceLocked(StationState& station) const;
    static float distanceNm(double lat1, double lon1, double lat2, double lon2);
    static float bearingDeg(double lat1, double lon1, double lat2, double lon2);

    mutable std::mutex mutex_;
    std::map<std::string, StationState> stations_;
    std::deque<LogEntry> log_;
    uint64_t total_packets_ = 0;

    bool station_valid_ = false;
    double station_lat_ = 0.0;
    double station_lon_ = 0.0;
};

}  // namespace aprs

#endif  // APRS_STORE_H
