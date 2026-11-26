#include "aprs/aprs_log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace aprs {
namespace {

constexpr size_t kMaxLoadedPackets = 20000;

}  // namespace

std::string Log::directory() {
    const char* home = std::getenv("HOME");
    std::string base = home != nullptr ? home : ".";
    return base + "/.local/share/zerosdr/aprs";
}

std::string Log::dateString(time_t when) {
    struct tm local {};
    localtime_r(&when, &local);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return buffer;
}

std::string Log::cutoffDate(int days) {
    return dateString(time(nullptr) - static_cast<time_t>(days) * 86400);
}

bool Log::append(const Packet& packet, uint32_t frequency, time_t when) {
    if (packet.source.empty()) return false;

    const std::string dir = directory();
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        std::cerr << "APRS log: cannot create " << dir << ": "
                  << error.message() << std::endl;
        return false;
    }

    nlohmann::json line;
    line["t"] = static_cast<long long>(when);
    line["freq"] = frequency;
    line["src"] = packet.source;
    line["dst"] = packet.destination;
    line["path"] = packet.path;
    line["info"] = packet.info;
    line["type"] = packet.type != 0 ? std::string(1, packet.type)
                                    : std::string();
    line["pos"] = packet.has_position;
    line["lat"] = packet.latitude;
    line["lon"] = packet.longitude;
    line["spd"] = packet.speed_knots;
    line["crs"] = packet.course;
    line["alt"] = packet.altitude_ft;
    line["sym"] = std::string(1, packet.symbol);
    line["tbl"] = std::string(1, packet.symbol_table);
    line["comment"] = packet.comment;
    line["wx"] = packet.weather;
    line["wdir"] = packet.wind_dir;
    line["wspd"] = packet.wind_speed_mph;
    line["gust"] = packet.wind_gust_mph;
    line["temp"] = packet.temperature_c;
    line["hastemp"] = packet.has_temperature;
    line["hum"] = packet.humidity;
    line["pres"] = packet.pressure_hpa;
    line["r1"] = packet.rain_hour_mm;
    line["r24"] = packet.rain_24h_mm;
    line["rmid"] = packet.rain_midnight_mm;
    line["msg"] = packet.message;
    line["addr"] = packet.addressee;
    line["mtext"] = packet.message_text;

    const std::string path = dir + "/" + dateString(when) + ".jsonl";
    std::ofstream file(path, std::ios::app);
    if (!file) {
        std::cerr << "APRS log: cannot open " << path << std::endl;
        return false;
    }
    file << line.dump() << '\n';
    return static_cast<bool>(file);
}

std::vector<std::pair<time_t, Packet>> Log::loadRecent(uint32_t frequency,
                                                       int days) const {
    pruneOlderThan(days);

    std::vector<std::pair<time_t, Packet>> records;
    const std::string dir = directory();
    std::error_code error;
    if (!std::filesystem::exists(dir, error)) return records;

    const std::string cutoff = cutoffDate(days);
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() < 6 ||
            name.compare(name.size() - 6, 6, ".jsonl") != 0) {
            continue;
        }
        const std::string stem = name.substr(0, name.size() - 6);
        if (stem.size() == 10 && stem >= cutoff) files.push_back(name);
    }
    if (error) return records;
    std::sort(files.begin(), files.end());

    for (const std::string& name : files) {
        std::ifstream file(dir + "/" + name);
        if (!file) continue;
        std::string text;
        while (std::getline(file, text)) {
            nlohmann::json line =
                nlohmann::json::parse(text, nullptr, false);
            if (line.is_discarded() || !line.is_object()) continue;
            if (line.value("freq", 0u) != frequency) continue;

            Packet packet;
            packet.source = line.value("src", std::string());
            if (packet.source.empty()) continue;
            packet.destination = line.value("dst", std::string());
            if (line.contains("path") && line["path"].is_array()) {
                for (const auto& item : line["path"]) {
                    if (item.is_string()) {
                        packet.path.push_back(item.get<std::string>());
                    }
                }
            }
            packet.info = line.value("info", std::string());
            const std::string type = line.value("type", std::string());
            packet.type = type.empty() ? 0 : type[0];
            packet.has_position = line.value("pos", false);
            packet.latitude = line.value("lat", 0.0);
            packet.longitude = line.value("lon", 0.0);
            packet.speed_knots = line.value("spd", -1.0f);
            packet.course = line.value("crs", -1.0f);
            packet.altitude_ft = line.value("alt", -100000);
            const std::string symbol = line.value("sym", std::string());
            const std::string table = line.value("tbl", std::string());
            packet.symbol = symbol.empty() ? '>' : symbol[0];
            packet.symbol_table = table.empty() ? '/' : table[0];
            packet.comment = line.value("comment", std::string());
            packet.weather = line.value("wx", false);
            packet.wind_dir = line.value("wdir", -1.0f);
            packet.wind_speed_mph = line.value("wspd", -1.0f);
            packet.wind_gust_mph = line.value("gust", -1.0f);
            packet.temperature_c = line.value("temp", 0.0f);
            packet.has_temperature = line.value("hastemp", false);
            packet.humidity = line.value("hum", -1.0f);
            packet.pressure_hpa = line.value("pres", -1.0f);
            packet.rain_hour_mm = line.value("r1", -1.0f);
            packet.rain_24h_mm = line.value("r24", -1.0f);
            packet.rain_midnight_mm = line.value("rmid", -1.0f);
            packet.message = line.value("msg", false);
            packet.addressee = line.value("addr", std::string());
            packet.message_text = line.value("mtext", std::string());

            const time_t when =
                static_cast<time_t>(line.value("t", (long long)time(nullptr)));
            records.emplace_back(when, std::move(packet));
        }
    }

    if (records.size() > kMaxLoadedPackets) {
        records.erase(records.begin(),
                      records.begin() +
                          static_cast<long>(records.size() - kMaxLoadedPackets));
    }
    return records;
}

void Log::pruneOlderThan(int days) const {
    const std::string dir = directory();
    std::error_code error;
    if (!std::filesystem::exists(dir, error)) return;

    const std::string cutoff = cutoffDate(days);
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() < 6 ||
            name.compare(name.size() - 6, 6, ".jsonl") != 0) {
            continue;
        }
        const std::string stem = name.substr(0, name.size() - 6);
        if (stem.size() == 10 && stem < cutoff) {
            std::error_code remove_error;
            std::filesystem::remove(entry.path(), remove_error);
        }
    }
}

}  // namespace aprs
