#include "app_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

std::string homeDir() {
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr && *home != '\0' ? home : "/home/pi");
}

std::string configDir() { return homeDir() + "/.config/zerosdr"; }

nlohmann::json readJson(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return nlohmann::json();
    try {
        nlohmann::json value;
        in >> value;
        return value;
    } catch (...) {
        std::cerr << "AppConfig: cannot parse " << path << std::endl;
        return nlohmann::json();
    }
}

}  // namespace

AppConfig::AppConfig() : path_(configDir() + "/config.json") {}

AppConfig& AppConfig::instance() {
    static AppConfig config;
    return config;
}

void AppConfig::load() {
    data_ = nlohmann::json::object();

    std::error_code ec;
    const bool existed = std::filesystem::exists(path_, ec);
    if (existed) {
        const nlohmann::json loaded = readJson(path_);
        if (loaded.is_object()) {
            data_ = loaded;
        } else {
            std::cerr << "AppConfig: ignoring invalid " << path_
                      << ", using defaults" << std::endl;
        }
    } else {
        migrateLegacyFiles();
    }

    setDefaults();
    std::cout << "AppConfig: " << (existed ? "loaded " : "created ")
              << path_ << std::endl;
    save();
}

void AppConfig::setDefaults() {
    auto section = [&](const char* name) -> nlohmann::json& {
        if (!data_.contains(name) || !data_[name].is_object()) {
            data_[name] = nlohmann::json::object();
        }
        return data_[name];
    };

    nlohmann::json& system = section("system");
    if (!system.contains("guide_show")) system["guide_show"] = true;

    nlohmann::json& sdr = section("sdr");
    if (!sdr.contains("deemphasis_us")) sdr["deemphasis_us"] = 50;
    if (!sdr.contains("audio_agc")) sdr["audio_agc"] = true;
    if (!sdr.contains("fft_size")) sdr["fft_size"] = 1024;
    if (!sdr.contains("sample_rate")) sdr["sample_rate"] = 1024000;
    if (!sdr.contains("theme")) sdr["theme"] = 0;

    nlohmann::json& spectrum = section("spectrum");
    if (!spectrum.contains("configured")) spectrum["configured"] = false;
    if (!spectrum.contains("start_hz")) spectrum["start_hz"] = 88000000;
    if (!spectrum.contains("stop_hz")) spectrum["stop_hz"] = 108000000;

    nlohmann::json& adsb = section("adsb");
    if (!adsb.contains("observer_latitude")) adsb["observer_latitude"] = 0.0;
    if (!adsb.contains("observer_longitude")) adsb["observer_longitude"] = 0.0;
    if (!adsb.contains("manual_override")) adsb["manual_override"] = false;
}

void AppConfig::migrateLegacyFiles() {
    const std::string dir = configDir();
    bool migrated = false;

    const nlohmann::json sdr = readJson(dir + "/sdr_settings.json");
    if (sdr.is_object()) {
        data_["sdr"]["deemphasis_us"] = sdr.value("deemphasis_us", 50);
        data_["sdr"]["audio_agc"] = sdr.value("audio_agc", true);
        data_["sdr"]["fft_size"] = sdr.value("fft_size", 1024);
        data_["sdr"]["sample_rate"] = sdr.value("sample_rate", 1024000);
        data_["sdr"]["theme"] = sdr.value("theme", 0);
        migrated = true;
    }

    {
        std::ifstream in(dir + "/spectrum_range.txt");
        uint32_t start = 0;
        uint32_t stop = 0;
        if (in >> start >> stop && start >= 24000000 &&
            stop <= 1766000000 && stop > start + 500000) {
            data_["spectrum"]["configured"] = true;
            data_["spectrum"]["start_hz"] = start;
            data_["spectrum"]["stop_hz"] = stop;
            migrated = true;
        }
    }

    const nlohmann::json adsb = readJson(dir + "/adsb_config.json");
    if (adsb.is_object()) {
        data_["adsb"]["observer_latitude"] =
            adsb.value("observer_latitude", 0.0);
        data_["adsb"]["observer_longitude"] =
            adsb.value("observer_longitude", 0.0);
        data_["adsb"]["manual_override"] =
            adsb.value("manual_override", false);
        migrated = true;
    }

    const nlohmann::json guide = readJson(dir + "/guide.json");
    if (guide.is_object()) {
        data_["system"]["guide_show"] = guide.value("show", true);
        migrated = true;
    }

    if (migrated) {
        std::cout << "AppConfig: imported legacy settings from " << dir
                  << std::endl;
    }
}

bool AppConfig::save() {
    std::error_code ec;
    const std::filesystem::path target(path_);
    std::filesystem::create_directories(target.parent_path(), ec);

    const std::string temp = path_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "AppConfig: cannot write " << temp << std::endl;
            return false;
        }
        out << data_.dump(2) << "\n";
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::cerr << "AppConfig: cannot replace " << path_ << ": "
                  << ec.message() << std::endl;
        return false;
    }
    return true;
}
