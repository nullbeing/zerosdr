#include "aprs/aprs_log.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

}  // namespace

int main() {
    setenv("HOME", "/tmp/opencode/aprs-log-home", 1);
    std::filesystem::remove_all(aprs::Log::directory());

    aprs::Packet packet;
    packet.source = "N0CALL-9";
    packet.destination = "APRS";
    packet.path = {"WIDE1-1", "WIDE2-1"};
    packet.type = '!';
    packet.has_position = true;
    packet.latitude = 23.12345;
    packet.longitude = 113.54321;
    packet.speed_knots = 32.0f;
    packet.course = 75.0f;
    packet.altitude_ft = 120;
    packet.symbol_table = '/';
    packet.symbol = '>';
    packet.comment = "test comment";
    packet.weather = true;
    packet.wind_dir = 220.0f;
    packet.wind_speed_mph = 12.0f;
    packet.wind_gust_mph = 18.0f;
    packet.temperature_c = 25.5f;
    packet.has_temperature = true;
    packet.humidity = 65.0f;
    packet.pressure_hpa = 1013.2f;
    packet.rain_hour_mm = 1.2f;
    packet.rain_24h_mm = 5.4f;
    packet.rain_midnight_mm = 2.1f;
    packet.info = "!2312.34N/11354.32E>test comment";

    aprs::Log log;
    const time_t now = time(nullptr);
    bool ok = true;
    ok = check(log.append(packet, 144640000, now), "append");
    auto records = log.loadRecent(144640000, 7);
    ok = check(records.size() == 1, "one restored record");
    if (!records.empty()) {
        const aprs::Packet& restored = records[0].second;
        ok = check(restored.source == "N0CALL-9", "source");
        ok = check(restored.path.size() == 2, "path");
        ok = check(std::fabs(restored.latitude - 23.12345) < 1e-9, "lat");
        ok = check(std::fabs(restored.longitude - 113.54321) < 1e-9, "lon");
        ok = check(std::fabs(restored.speed_knots - 32.0f) < 0.01f, "speed");
        ok = check(restored.symbol == '>', "symbol");
        ok = check(restored.weather, "weather");
        ok = check(std::fabs(restored.wind_gust_mph - 18.0f) < 0.01f, "gust");
        ok = check(restored.has_temperature, "temperature flag");
        ok = check(std::fabs(restored.pressure_hpa - 1013.2f) < 0.01f,
                   "pressure");
    }

    auto other = log.loadRecent(144800000, 7);
    ok = check(other.empty(), "frequency filter");

    aprs::Packet old;
    old.source = "OLD-1";
    old.info = "old";
    ok = check(log.append(old, 144640000, now - 8 * 86400), "append old");
    auto pruned = log.loadRecent(144640000, 7);
    ok = check(pruned.size() == 1, "old record pruned");
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             aprs::Log::directory())) {
        if (entry.is_regular_file()) ++files;
    }
    ok = check(files == 1, "one daily file after prune");

    std::filesystem::remove_all(aprs::Log::directory());
    if (!ok) {
        std::fprintf(stderr, "APRS log test FAILED\n");
        return 1;
    }
    std::printf("APRS log test OK\n");
    return 0;
}
