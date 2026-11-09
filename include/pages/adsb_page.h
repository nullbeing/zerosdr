#ifndef ADSB_PAGE_H
#define ADSB_PAGE_H

#include "core/ipage.h"
#include "framebuffer.h"
#include "sdr_controller.h"
#include "ui/adsb_ui_renderer.h"
#include "ui/radar_ui_renderer.h"
#include "adsb/adsb_message.h"
#include "adsb/readsb_controller.h"
#include "adsb/readsb_json_parser.h"
#include "adsb/adsb_config.h"
#include <vector>
#include <map>
#include <memory>
#include <string>

/**
 * ADSBPage - ADSB receiver page
 *
 * Displays aircraft information decoded from 1090 MHz ADS-B signals
 * Features:
 * - Table-based aircraft list with scrolling
 * - Real-time message decoding
 * - Aircraft tracking with timeout
 * - Detail view for selected aircraft
 * - Radar view for spatial visualization
 */
class ADSBPage : public IPage {
public:
    ADSBPage(Framebuffer& fb, SDRController& sdr, ADSBUIRenderer& ui, RadarUIRenderer& radar_ui);
    ~ADSBPage();

    // IPage interface implementation
    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    const char* getName() const override { return "ADSB"; }
    const char* getDescription() const override { return "ADSB Aircraft Tracker"; }

private:
    // References to shared services
    Framebuffer& fb_;
    SDRController& sdr_;
    ADSBUIRenderer& ui_;
    RadarUIRenderer& radar_ui_;

    // Aircraft database
    std::map<uint32_t, AircraftState> aircraft_db_;
    std::vector<AircraftState> aircraft_list_;  // Sorted list for display

    // UI state
    enum ViewMode {
        VIEW_LIST,
        VIEW_DETAIL,
        VIEW_RADAR,
        VIEW_SETTINGS
    };
    ViewMode view_mode_;
    int selected_index_;
    int scroll_offset_;
    int detail_scroll_offset_;  // Scroll offset for detail view
    uint32_t selected_icao_;  // ICAO address of selected aircraft in detail view

    // Radar state
    float sweep_angle_;           // Current sweep angle (0-360°)
    float sweep_speed_;           // Degrees per second (60°/s = 6s per rotation)
    bool show_position_prompt_;   // Show "position not set" dialog in radar view
    float blink_timer_;           // Timer for blinking selected aircraft
    bool blink_state_;            // Current blink state (true = visible)

    // Settings menu state
    int settings_selected_item_;  // 0=latitude, 1=longitude, 2=save, 3=cancel
    std::string lat_input_;
    std::string lon_input_;
    bool editing_lat_;
    bool editing_lon_;

    // Statistics
    uint32_t total_messages_;
    uint32_t crc_errors_;
    float message_rate_;
    time_t last_rate_update_;
    uint32_t messages_since_update_;

    // Configuration
    int gain_;
    double observer_lat_;
    double observer_lon_;
    bool observer_position_set_;

    // Sample buffer
    std::vector<uint8_t> samples_;

    // readsb integration
    std::unique_ptr<READSBController> readsb_;
    READSBJSONParser json_parser_;
    std::string json_path_;
    time_t last_json_read_;

    // Configuration
    ADSBConfig config_;
    std::string config_path_;

    // Helper methods
    void updateAircraftList();
    void removeStaleAircraft();
    void sortAircraftList();
    void calculateDistance(AircraftState& aircraft);
    float calculateMaxRange();
    void renderCurrentView();
    void handleListInput(AppKeyCode key);
    void handleDetailInput(AppKeyCode key);
    void handleRadarInput(AppKeyCode key);
    void handleSettingsInput(AppKeyCode key);
    void renderSettingsMenu();
    void loadConfig();
    void saveConfig();

    // Test data generation (for Phase 1)
    void generateTestData();
};

#endif // ADSB_PAGE_H
