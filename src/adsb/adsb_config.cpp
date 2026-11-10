#include "adsb/adsb_config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cstdlib>

ADSBConfig::ADSBConfig()
    : observer_lat_(0.0),
      observer_lon_(0.0),
      observer_position_set_(false),
      manual_override_(false) {
}

ADSBConfig::~ADSBConfig() {
}

bool ADSBConfig::load(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        std::cout << "ADSBConfig: No config file found at " << config_path << std::endl;
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    if (!parseConfigFile(content)) {
        std::cerr << "ADSBConfig: Failed to parse config file" << std::endl;
        return false;
    }

    std::cout << "ADSBConfig: Loaded configuration from " << config_path << std::endl;
    if (observer_position_set_) {
        std::cout << "  Observer position: " << observer_lat_ << ", " << observer_lon_ << std::endl;
    }

    return true;
}

bool ADSBConfig::save(const std::string& config_path) {
    std::string content = generateConfigFile();

    std::ofstream file(config_path);
    if (!file.is_open()) {
        std::cerr << "ADSBConfig: Failed to open config file for writing: " << config_path << std::endl;
        return false;
    }

    file << content;
    file.close();

    std::cout << "ADSBConfig: Saved configuration to " << config_path << std::endl;
    return true;
}

void ADSBConfig::setObserverPosition(double lat, double lon) {
    observer_lat_ = lat;
    observer_lon_ = lon;
    observer_position_set_ = true;
}

void ADSBConfig::clearObserverPosition() {
    observer_lat_ = 0.0;
    observer_lon_ = 0.0;
    observer_position_set_ = false;
}

bool ADSBConfig::parseConfigFile(const std::string& content) {
    // Simple JSON parser for our specific format
    // Expected format:
    // {
    //   "observer_latitude": 37.7749,
    //   "observer_longitude": -122.4194
    // }

    const char* json = content.c_str();

    bool lat_ok = false;
    bool lon_ok = false;

    // Find observer_latitude
    const char* lat_field = strstr(json, "\"observer_latitude\"");
    if (lat_field) {
        const char* colon = strchr(lat_field, ':');
        if (colon) {
            observer_lat_ = atof(colon + 1);
            lat_ok = true;
        }
    }

    // Find observer_longitude
    const char* lon_field = strstr(json, "\"observer_longitude\"");
    if (lon_field) {
        const char* colon = strchr(lon_field, ':');
        if (colon) {
            observer_lon_ = atof(colon + 1);
            lon_ok = true;
        }
    }

    // A half-written file must not create a fake position, and an all-zero
    // pair is what "clear position" writes, so it does not count as set.
    const bool all_zero = observer_lat_ == 0.0 && observer_lon_ == 0.0;
    observer_position_set_ = lat_ok && lon_ok && !all_zero &&
                             observer_lat_ >= -90.0 && observer_lat_ <= 90.0 &&
                             observer_lon_ >= -180.0 && observer_lon_ <= 180.0;

    // Manual override flag (older files do not have it).
    const char* manual_field = strstr(json, "\"manual_override\"");
    if (manual_field) {
        const char* colon = strchr(manual_field, ':');
        manual_override_ = colon && strstr(colon, "true") != nullptr;
    }

    return observer_position_set_;
}

std::string ADSBConfig::generateConfigFile() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"observer_latitude\": " << observer_lat_ << ",\n";
    oss << "  \"observer_longitude\": " << observer_lon_ << ",\n";
    oss << "  \"manual_override\": " << (manual_override_ ? "true" : "false") << "\n";
    oss << "}\n";
    return oss.str();
}
