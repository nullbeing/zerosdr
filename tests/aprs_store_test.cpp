#include "aprs/aprs_store.h"

#include <cmath>
#include <cstdio>

namespace {

aprs::Packet position(const char* source, double latitude, double longitude,
                      float speed = 20.0f, float course = 90.0f) {
    aprs::Packet packet;
    packet.source = source;
    packet.destination = "APRS";
    packet.type = '!';
    packet.has_position = true;
    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.speed_knots = speed;
    packet.course = course;
    packet.symbol_table = '/';
    packet.symbol = '>';
    packet.info = "!test";
    return packet;
}

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

}  // namespace

int main() {
    aprs::Store store;
    store.setStation(23.1291, 113.2644, true);

    // Five moving positions build five trail points.
    for (int i = 0; i < 5; ++i) {
        store.apply(position("N0CALL-9", 23.1200 + i * 0.001,
                             113.2500 + i * 0.001));
    }
    auto stations = store.snapshot();
    bool ok = true;
    ok = check(stations.size() == 1, "one station");
    ok = check(stations[0].trail.size() == 5, "five trail points");
    ok = check(std::fabs(stations[0].trail.front().latitude - 23.1200) < 1e-9,
               "first trail point");
    ok = check(std::fabs(stations[0].trail.back().longitude - 113.2540) < 1e-9,
               "last trail point");
    ok = check(stations[0].distance_nm > 0.0f, "distance from observer");

    // A duplicate and a GPS-jitter-sized move do not add points.
    store.apply(position("N0CALL-9", 23.1240, 113.2540));
    store.apply(position("N0CALL-9", 23.12405, 113.25405));
    stations = store.snapshot();
    ok = check(stations[0].trail.size() == 5, "duplicates and jitter ignored");

    // A real move adds one point.
    store.apply(position("N0CALL-9", 23.12430, 113.25430));
    stations = store.snapshot();
    ok = check(stations[0].trail.size() == 6, "real move adds a point");

    // A positionless weather packet updates the station but not the track.
    aprs::Packet weather;
    weather.source = "N0CALL-9";
    weather.type = '_';
    weather.weather = true;
    weather.wind_dir = 220.0f;
    weather.wind_speed_mph = 12.0f;
    weather.temperature_c = 25.5f;
    weather.info = "_10090556c220s012t077";
    store.apply(weather);
    stations = store.snapshot();
    ok = check(stations[0].trail.size() == 6, "weather keeps the track");
    ok = check(stations[0].weather, "weather flag");
    ok = check(std::fabs(stations[0].wind_dir - 220.0f) < 0.1f,
               "weather wind");

    if (!ok) {
        std::fprintf(stderr, "APRS store test FAILED\n");
        return 1;
    }
    std::printf("APRS store test OK\n");
    return 0;
}
