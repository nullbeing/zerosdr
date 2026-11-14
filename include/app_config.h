#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <nlohmann/json.hpp>

#include <string>

// Single JSON configuration for the whole application:
//   $HOME/.config/zerosdr/config.json
//
// Sections:
//   system   - guide_show
//   sdr      - deemphasis_us, audio_agc, fft_size, sample_rate, theme
//   spectrum - configured, start_hz, stop_hz
//   adsb     - observer_latitude, observer_longitude, manual_override
//
// On first load the legacy sdr_settings.json / spectrum_range.txt /
// adsb_config.json / guide.json files are imported once.
class AppConfig {
public:
    static AppConfig& instance();

    void load();
    bool save();

    nlohmann::json& data() { return data_; }
    const nlohmann::json& data() const { return data_; }

    const std::string& path() const { return path_; }

private:
    AppConfig();

    void setDefaults();
    void migrateLegacyFiles();

    nlohmann::json data_ = nlohmann::json::object();
    std::string path_;
};

#endif  // APP_CONFIG_H
