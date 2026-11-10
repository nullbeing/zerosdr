#ifndef ADSB_CONFIG_H
#define ADSB_CONFIG_H

#include <string>

/**
 * ADSBConfig - Configuration manager for ADSB page
 *
 * Handles loading and saving observer location settings tile
 */
class ADSBConfig {
public:
    ADSBConfig();
    ~ADSBConfig();

    // Load configuration from file
    bool load(const std::string& config_path);

    // Save configuration to file
    bool save(const std::string& config_path);

    // Getters
    double getObserverLat() const { return observer_lat_; }
    double getObserverLon() const { return observer_lon_; }
    bool isObserverPositionSet() const { return observer_position_set_; }

    // When true the user entered the position by hand; a GPS fix must not
    // overwrite it until the override is cleared.
    bool isManualOverride() const { return manual_override_; }
    void setManualOverride(bool manual) { manual_override_ = manual; }

    // Setters
    void setObserverPosition(double lat, double lon);
    void clearObserverPosition();

private:
    double observer_lat_;
    double observer_lon_;
    bool observer_position_set_;
    bool manual_override_;

    // Helper methods for simple JSON parsing/writing
    bool parseConfigFile(const std::string& content);
    std::string generateConfigFile() const;
};

#endif // ADSB_CONFIG_H
