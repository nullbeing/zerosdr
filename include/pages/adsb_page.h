#ifndef ADSB_PAGE_H
#define ADSB_PAGE_H

#include "core/ipage.h"
#include "core/beep_player.h"
#include "core/gps_locator.h"
#include "framebuffer.h"
#include "sdr_controller.h"
#include "imu_compass.h"
#include "ui/adsb_ui_renderer.h"
#include "ui/radar_ui_renderer.h"
#include "adsb/adsb_message.h"
#include "adsb/readsb_controller.h"
#include "adsb/readsb_json_parser.h"
#include "adsb/adsb_config.h"
#include "adsb/map_background.h"
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <chrono>

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
class InputHandler;

class ADSBPage : public IPage {
public:
    ADSBPage(Framebuffer& fb, SDRController& sdr, InputHandler& input,
             ADSBUIRenderer& ui, RadarUIRenderer& radar_ui);
    ~ADSBPage();

    // IPage interface implementation
    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    void onVolumeChanged(int percent) override;
    void onShortcut(const char* id) override;
    const char* getName() const override { return "ADSB"; }
    const char* getDescription() const override { return "ADSB Aircraft Tracker"; }

private:
    // References to shared services
    Framebuffer& fb_;
    SDRController& sdr_;
    InputHandler& input_;
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
    AppKeyCode radar_held_key_ = APPKEY_NONE;
    float radar_repeat_accum_ = 0.0f;
    uint32_t selected_icao_;  // ICAO address of selected aircraft in detail view
    bool pfd_square_ = false; // PFD attitude shape: false = round, true = square
    int radar_theme_ = 0;     // 0 = green scope, 1 = red tech scope
    bool list_top_jump_ = false;  // long F/up already jumped to the top

    // Radar state
    float sweep_angle_;           // Current sweep angle (0-360°)
    float sweep_speed_;           // Degrees per second (60°/s = 6s per rotation)
    bool show_position_prompt_;   // Show "position not set" dialog in radar view
    float blink_timer_;           // Timer for blinking selected aircraft
    bool blink_state_;            // Current blink state (true = visible)
    int last_sweep_sector_;       // Radar sector (90 deg) for the tick sound

    // Compass (BMI270 + BMM150). When the board has no IMU the radar stays
    // north-up and the I key is a no-op.
    ImuCompass imu_;
    bool imu_enabled_;            // Poll the compass at all
    bool heading_up_;             // Rotate the scope so the heading is at the top
    float radar_rotation_;        // Smoothed rotation currently drawn
    float radar_zoom_;            // Animated scope scale (1.0 = right panel)
    float radar_zoom_target_;     // Scale requested by sym+T / sym+R
    float radar_zoom_velocity_;   // Spring velocity for the bounce
    float radar_view_age_;        // Seconds since the radar view was opened

    // Optional raster map under the scope (CARTO dark_nolabels by default).
    MapBackground map_bg_;

    // UI sounds
    BeepPlayer beeps_;
    std::set<uint32_t> known_icaos_;  // for the "new aircraft" chime
    time_t last_new_aircraft_chime_ = 0;  // throttle: at most one per 3 s

    // Enter-key view cycle helper
    void openRadarView();

    // GPS position source
    GpsLocator gps_;
    time_t last_gps_save_ = 0;

    // H overlay (key list + position settings)
    bool help_visible_ = false;
    int help_tab_ = 0;           // 0 = keys, 1 = settings
    int help_setting_index_ = 0; // 0 = latitude, 1 = longitude, 2 = clear
    ViewMode settings_return_view_ = VIEW_DETAIL;
    void handleHelpInput(AppKeyCode key);

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
    std::string volume_toast_;
    std::chrono::steady_clock::time_point volume_toast_until_{};
    time_t last_json_read_;
    time_t last_parse_time_ = 0;   // previous aircraft.json parse (bank estimate)
    time_t last_feed_restart_ = 0; // throttle readsb restarts on a stale feed

    // Estimated bank angle from the track change rate, kept per ICAO so it
    // survives the parser rebuilding the aircraft table every second.
    std::map<uint32_t, float> bank_deg_;
    std::map<uint32_t, int> prev_heading_;
    std::map<uint32_t, float> alt_display_;   // smoothed PFD altitude

    // Variometer cue for the PFD: a continuous tone whose pitch follows
    // the vertical speed and whose direction follows the sign.
    uint32_t vario_icao_ = 0;

    // Configuration
    ADSBConfig config_;
    std::string config_path_;

    // Helper methods
    void updateAircraftList();
    void removeStaleAircraft();
    void sortAircraftList();
    void calculateDistance(AircraftState& aircraft);
    void updateBankEstimate(AircraftState& aircraft, float delta_seconds);
    void updateVarioSound();
    float calculateMaxRange();
    void renderCurrentView();
    void handleListInput(AppKeyCode key);
    void handleDetailInput(AppKeyCode key);
    void handleRadarInput(AppKeyCode key, bool beep = true);
    void handleSettingsInput(AppKeyCode key);

    // Compass helpers
    void updateRadarRotation(float delta_time);
    void updateRadarZoom(float delta_time);
    void loadImuConfig();
    void saveImuConfig();

    // Transient fn+D / fn+S feedback drawn over the current view.
    void drawVolumeToast();
    void renderSettingsMenu();
    void loadConfig();
    void saveConfig();
    void cycleMapProvider();

    // Test data generation (for Phase 1)
    void generateTestData();
};

#endif // ADSB_PAGE_H
