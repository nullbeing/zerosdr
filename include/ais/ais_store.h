#ifndef AIS_STORE_H
#define AIS_STORE_H

#include "ais/ais_message.h"

#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <utility>
#include <mutex>
#include <string>
#include <vector>

namespace ais {

struct VesselState {
    uint32_t mmsi = 0;
    std::string name;
    std::string callsign;
    std::string destination;

    int last_type = 0;
    int ship_type = -1;
    bool class_b = false;
    bool aton = false;
    bool base_station = false;

    bool has_position = false;
    double latitude = 0.0;
    double longitude = 0.0;
    float sog = -1.0f;
    float cog = -1.0f;
    int heading = -1;
    int nav_status = -1;

    float distance_nm = -1.0f;
    float bearing_deg = 0.0f;

    // Closest point of approach for a stationary shore station.
    float cpa_nm = -1.0f;
    float tcpa_min = -1.0f;
    float cpa_east_nm = 0.0f;
    float cpa_north_nm = 0.0f;

    time_t last_seen = 0;
    time_t last_static = 0;
    uint32_t messages = 0;
    bool stale = false;

    // Last reported positions, oldest first. Used by the plot view.
    std::deque<std::pair<double, double>> trail;
};

struct AisLogEntry {
    time_t time = 0;
    char channel = 'A';
    int type = 0;
    uint32_t mmsi = 0;
    std::string text;
};

class AisStore {
public:
    void clear();
    void apply(const AisFrame& frame);

    /** Observer position for distance/bearing, in decimal degrees. */
    void setStation(double latitude, double longitude, bool valid);
    bool hasStation() const;
    double stationLatitude() const;
    double stationLongitude() const;

    std::vector<VesselState> snapshot();
    std::vector<AisLogEntry> logSnapshot() const;

    uint64_t totalMessages() const;
    size_t vesselCount() const;

    static const char* shipTypeLabel(int ship_type);

private:
    void pruneLocked(time_t now);
    void updateDistanceLocked(VesselState& vessel) const;
    void updateCpaLocked(VesselState& vessel) const;
    static float distanceNm(double lat1, double lon1, double lat2, double lon2);
    static float bearingDeg(double lat1, double lon1, double lat2, double lon2);

    mutable std::mutex mutex_;
    std::map<uint32_t, VesselState> vessels_;
    std::deque<AisLogEntry> log_;
    uint64_t total_messages_ = 0;

    bool station_valid_ = false;
    double station_lat_ = 0.0;
    double station_lon_ = 0.0;
};

}  // namespace ais

#endif  // AIS_STORE_H
