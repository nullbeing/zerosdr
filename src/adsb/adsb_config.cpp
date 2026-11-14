#include "adsb/adsb_config.h"
#include "app_config.h"
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
    (void)config_path;
    const auto& data = AppConfig::instance().data();
    if (!data.contains("adsb") || !data["adsb"].is_object()) {
        return false;
    }
    const auto& adsb = data["adsb"];

    observer_lat_ = adsb.value("observer_latitude", 0.0);
    observer_lon_ = adsb.value("observer_longitude", 0.0);
    manual_override_ = adsb.value("manual_override", false);

    const bool all_zero = observer_lat_ == 0.0 && observer_lon_ == 0.0;
    observer_position_set_ =
        !all_zero && observer_lat_ >= -90.0 && observer_lat_ <= 90.0 &&
        observer_lon_ >= -180.0 && observer_lon_ <= 180.0;

    std::cout << "ADSBConfig: loaded " << AppConfig::instance().path()
              << std::endl;
    if (observer_position_set_) {
        std::cout << "  Observer position: " << observer_lat_ << ", "
                  << observer_lon_ << std::endl;
    }
    return observer_position_set_;
}

bool ADSBConfig::save(const std::string& config_path) {
    (void)config_path;
    auto& data = AppConfig::instance().data();
    if (!data.contains("adsb") || !data["adsb"].is_object()) {
        data["adsb"] = nlohmann::json::object();
    }
    auto& adsb = data["adsb"];
    adsb["observer_latitude"] = observer_lat_;
    adsb["observer_longitude"] = observer_lon_;
    adsb["manual_override"] = manual_override_;
    return AppConfig::instance().save();
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
