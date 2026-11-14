#include "pages/adsb_page.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <unistd.h>

namespace {
// Prefer the decoder bundled in the package (built with the RTL-SDR backend),
// then fall back to a system-wide install.
std::string findReadsbPath() {
    static const char* kCandidates[] = {
        "/usr/lib/zerosdr/readsb",
        "/usr/bin/readsb",
        "/usr/local/bin/readsb",
    };
    for (const char* path : kCandidates) {
        if (access(path, X_OK) == 0) return path;
    }
    return kCandidates[1];
}
}  // namespace

ADSBPage::ADSBPage(Framebuffer& fb, SDRController& sdr, ADSBUIRenderer& ui, RadarUIRenderer& radar_ui)
    : fb_(fb), sdr_(sdr), ui_(ui), radar_ui_(radar_ui),
      view_mode_(VIEW_LIST),
      selected_index_(0),
      scroll_offset_(0),
      detail_scroll_offset_(0),
      selected_icao_(0),
      sweep_angle_(0.0f),
      sweep_speed_(60.0f),  // 60°/s = 6 seconds per rotation
      show_position_prompt_(false),
      blink_timer_(0.0f),
      blink_state_(true),
      last_sweep_sector_(-1),
      settings_selected_item_(0),
      editing_lat_(false),
      editing_lon_(false),
      total_messages_(0),
      crc_errors_(0),
      message_rate_(0.0f),
      last_rate_update_(0),
      messages_since_update_(0),
      gain_(-1),
      observer_lat_(0.0),
      observer_lon_(0.0),
      observer_position_set_(false),
      last_json_read_(0) {
    const char* home = getenv("HOME");
    config_path_ = std::string(home ? home : "/home/pi") +
                   "/.config/zerosdr/adsb_config.json";
}

ADSBPage::~ADSBPage() {
}

bool ADSBPage::init() {
    std::cout << "ADSBPage: Initializing..." << std::endl;
    // Load configuration
    loadConfig();
    return true;
}

void ADSBPage::cleanup() {
    std::cout << "ADSBPage: Cleaning up..." << std::endl;

    // Stop readsb on cleanup (program exit)
    if (readsb_) {
        readsb_->stop();
        readsb_.reset();
    }

    aircraft_db_.clear();
    aircraft_list_.clear();
}

void ADSBPage::onEnter() {
    std::cout << "ADSBPage: Entering ADSB mode" << std::endl;

    // Check if readsb is already running (e.g., from Radar page)
    // If so, reuse it instead of restarting
    const std::string readsb_path = findReadsbPath();
    if (!readsb_) {
        readsb_ = std::make_unique<READSBController>(
            readsb_path,
            "/tmp/readsb/"
        );
    }

    if (access(readsb_path.c_str(), X_OK) != 0) {
        std::cerr << "ADSBPage: readsb binary not found at " << readsb_path
                  << " - ADSB decoding disabled" << std::endl;
    } else if (!readsb_->isRunning()) {
        std::cout << "ADSBPage: readsb not running, starting it..." << std::endl;

        // Stop zerosdr's SDR capture and close device to release RTL-SDR for readsb
        if (sdr_.isCapturing()) {
            sdr_.stopCapture();
            std::cout << "ADSBPage: Stopped SDR capture" << std::endl;
        }

        // Close SDR device completely to release it
        sdr_.close();
        std::cout << "ADSBPage: Closed SDR device" << std::endl;

        // Give device time to be released
        usleep(500000);  // 500ms

        int gain_value = (gain_ >= 0) ? (gain_ / 10) : -1;  // -1 = readsb auto gain
        if (!readsb_->start(gain_value)) {
            std::cerr << "ADSBPage: Failed to start readsb" << std::endl;
        } else {
            std::cout << "ADSBPage: readsb started" << std::endl;
        }
    } else {
        std::cout << "ADSBPage: readsb already running, reusing it" << std::endl;
    }

    json_path_ = readsb_->getJSONPath();
    last_json_read_ = 0;
    aircraft_db_.clear();
    known_icaos_.clear();
    loadConfig();  // pick up position changes made from the guide

    // Reset view state
    view_mode_ = VIEW_LIST;
    selected_index_ = 0;
    scroll_offset_ = 0;

    // Clear screen and render initial view
    fb_.clear(COLOR_BG_DARK);
    renderCurrentView();
    fb_.update();
}

void ADSBPage::onExit() {
    std::cout << "ADSBPage: Exiting ADSB mode" << std::endl;
    beeps_.close();

    // Don't stop readsb or reset the pointer - let it keep running
    // If switching to Radar page, it will reuse the same readsb instance
    // If switching to SDR page, SDRPage will handle stopping readsb if needed

    std::cout << "ADSBPage: Keeping readsb running for potential reuse" << std::endl;
}

void ADSBPage::update(float delta_time) {
    time_t now = time(nullptr);

    // GPS is the position source unless the user entered one by hand.
    gps_.poll();
    if (gps_.hasFix() && !config_.isManualOverride()) {
        const double lat = gps_.latitude();
        const double lon = gps_.longitude();
        const bool moved = !observer_position_set_ ||
                           std::fabs(lat - observer_lat_) > 1e-5 ||
                           std::fabs(lon - observer_lon_) > 1e-5;
        if (moved) {
            observer_lat_ = lat;
            observer_lon_ = lon;
            observer_position_set_ = true;
        }
        if (now != last_gps_save_ && now - last_gps_save_ >= 60) {
            saveConfig();
            last_gps_save_ = now;
        }
    }

    // Update radar sweep angle if in radar view
    if (view_mode_ == VIEW_RADAR) {
        sweep_angle_ += sweep_speed_ * delta_time;
        bool wrapped = false;
        if (sweep_angle_ >= 360.0f) {
            sweep_angle_ -= 360.0f;
            wrapped = true;
            // One sonar ping per revolution.
            beeps_.play(880.0f, 90, 0.20f);
            last_sweep_sector_ = -1;
        }
        if (!wrapped) {
            // Quiet tick every 90 degrees while the sweep turns.
            const int sector = static_cast<int>(sweep_angle_ / 90.0f);
            if (sector != last_sweep_sector_) {
                last_sweep_sector_ = sector;
                beeps_.play(1568.0f, 12, 0.045f);
            }
        }

        // Update blink timer (blink every 0.5 seconds)
        blink_timer_ += delta_time;
        if (blink_timer_ >= 0.5f) {
            blink_timer_ -= 0.5f;
            blink_state_ = !blink_state_;
        }
    }

    // Read aircraft.json every second
    if (now != last_json_read_) {
        if (readsb_ && readsb_->isRunning()) {
            if (json_parser_.parseFile(json_path_, aircraft_db_)) {
                last_json_read_ = now;

                // Chime when a new ICAO address shows up.
                const size_t known_before = known_icaos_.size();
                for (const auto& pair : aircraft_db_) {
                    known_icaos_.insert(pair.first);
                }
                // Throttle: a busy sky would otherwise chime every second.
                if (known_icaos_.size() > known_before &&
                    now - last_new_aircraft_chime_ >= 3) {
                    last_new_aircraft_chime_ = now;
                    beeps_.play(1200.0f, 45, 0.16f);
                    beeps_.play(1600.0f, 55, 0.16f);
                }

                // Calculate distance and accumulate total messages
                uint32_t new_total = 0;
                for (auto& pair : aircraft_db_) {
                    calculateDistance(pair.second);
                    new_total += pair.second.message_count;
                }

                // Update message rate from total delta
                messages_since_update_ += new_total - total_messages_;
                total_messages_ = new_total;
            }
        }
    }

    // Update message rate calculation
    if (now != last_rate_update_) {
        message_rate_ = (float)messages_since_update_ / (now - last_rate_update_);
        messages_since_update_ = 0;
        last_rate_update_ = now;
    }

    // Remove stale aircraft (>60s since last message)
    removeStaleAircraft();

    // Update aircraft list for display
    updateAircraftList();

    // Render current view
    renderCurrentView();
    fb_.update();
}

void ADSBPage::openRadarView() {
    if (observer_position_set_) {
        view_mode_ = VIEW_RADAR;
        sweep_angle_ = 0.0f;
        last_sweep_sector_ = -1;
        std::cout << "ADSBPage: Switching to radar view" << std::endl;
    } else {
        settings_return_view_ = VIEW_RADAR;
        view_mode_ = VIEW_SETTINGS;
        settings_selected_item_ = 0;
        lat_input_.clear();
        lon_input_.clear();
        std::cout << "ADSBPage: Observer position not set, opening settings"
                  << std::endl;
    }
}

bool ADSBPage::handleInput(AppKeyCode key) {
    if (help_visible_) {
        handleHelpInput(key);
        return true;
    }
    if (key == APPKEY_H) {
        help_visible_ = true;
        help_tab_ = 0;
        help_setting_index_ = 0;
        return true;
    }

    if (view_mode_ == VIEW_LIST) {
        handleListInput(key);
    } else if (view_mode_ == VIEW_DETAIL) {
        handleDetailInput(key);
    } else if (view_mode_ == VIEW_RADAR) {
        handleRadarInput(key);
    } else if (view_mode_ == VIEW_SETTINGS) {
        handleSettingsInput(key);
    }
    return true;
}

void ADSBPage::handleListInput(AppKeyCode key) {
    switch (key) {
        case APPKEY_UP:
            if (selected_index_ > 0) {
                selected_index_--;
                beeps_.play(1000.0f, 15, 0.06f);
                // Adjust scroll if needed
                if (selected_index_ < scroll_offset_) {
                    scroll_offset_ = selected_index_;
                }
            }
            break;

        case APPKEY_DOWN:
            if (selected_index_ < (int)aircraft_list_.size() - 1) {
                selected_index_++;
                beeps_.play(1000.0f, 15, 0.06f);
                // Adjust scroll if needed
                int max_visible = 10;  // MAX_VISIBLE_ROWS from renderer
                if (selected_index_ >= scroll_offset_ + max_visible) {
                    scroll_offset_ = selected_index_ - max_visible + 1;
                }
            }
            break;

        // No PgUp/PgDn on the Cardputer keyboard: Left/Right page the list.
        case APPKEY_LEFT:
            selected_index_ = std::max(0, selected_index_ - 5);
            scroll_offset_ = std::max(0, scroll_offset_ - 5);
            beeps_.play(1320.0f, 15, 0.06f);
            break;

        case APPKEY_RIGHT:
            selected_index_ = std::min((int)aircraft_list_.size() - 1, selected_index_ + 5);
            scroll_offset_ = std::min((int)aircraft_list_.size() - 10, scroll_offset_ + 5);
            if (scroll_offset_ < 0) scroll_offset_ = 0;
            beeps_.play(1320.0f, 15, 0.06f);
            break;

        case APPKEY_ENTER:
            if (!aircraft_list_.empty() && selected_index_ >= 0 &&
                selected_index_ < (int)aircraft_list_.size()) {
                view_mode_ = VIEW_DETAIL;
                selected_icao_ = aircraft_list_[selected_index_].icao_addr;
                detail_scroll_offset_ = 0;
                std::cout << "ADSBPage: Switching to detail view for aircraft "
                          << std::hex << selected_icao_ << std::dec << std::endl;
            } else {
                // Nothing selected: Enter continues the page cycle.
                openRadarView();
            }
            break;

        case APPKEY_R:
            // Direct shortcut to the radar view.
            openRadarView();
            break;

        case APPKEY_SCREENSHOT: {
            char filename[64];
            time_t now = time(nullptr);
            struct tm* t = localtime(&now);
            snprintf(filename, sizeof(filename),
                     "/tmp/adsb_%04d%02d%02d_%02d%02d%02d.png",
                     t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec);
            if (fb_.saveScreenshot(filename)) {
                std::cout << "Screenshot saved: " << filename << std::endl;
            } else {
                std::cerr << "Screenshot failed" << std::endl;
            }
            break;
        }

        case APPKEY_G:
            // Toggle gain mode
            if (gain_ < 0) {
                gain_ = 0;
                sdr_.setAutoGain(false);
                sdr_.setGain(gain_);
                std::cout << "ADSBPage: Manual gain mode (0 dB)" << std::endl;
            } else {
                gain_ = -1;
                sdr_.setAutoGain(true);
                std::cout << "ADSBPage: Auto gain mode" << std::endl;
            }
            break;

        case APPKEY_PLUS:
            if (gain_ >= 0 && gain_ < 490) {
                gain_ += 10;
                sdr_.setGain(gain_);
                std::cout << "ADSBPage: Gain = " << gain_/10 << " dB" << std::endl;
            }
            break;

        case APPKEY_MINUS:
            if (gain_ >= 0 && gain_ > 0) {
                gain_ -= 10;
                sdr_.setGain(gain_);
                std::cout << "ADSBPage: Gain = " << gain_/10 << " dB" << std::endl;
            }
            break;

        default:
            break;
    }
}

void ADSBPage::handleDetailInput(AppKeyCode key) {
    switch (key) {
        case APPKEY_UP:
            if (detail_scroll_offset_ > 0) {
                detail_scroll_offset_ -= 15;  // Scroll up by one line
            }
            break;

        case APPKEY_DOWN:
            detail_scroll_offset_ += 15;  // Scroll down by one line
            break;

        case APPKEY_LEFT:
            // Switch to previous aircraft
            if (!aircraft_list_.empty()) {
                // Find current aircraft in list
                int current_idx = -1;
                for (size_t i = 0; i < aircraft_list_.size(); i++) {
                    if (aircraft_list_[i].icao_addr == selected_icao_) {
                        current_idx = i;
                        break;
                    }
                }

                if (current_idx > 0) {
                    // Move to previous aircraft
                    selected_icao_ = aircraft_list_[current_idx - 1].icao_addr;
                    selected_index_ = current_idx - 1;
                    detail_scroll_offset_ = 0;  // Reset scroll for new aircraft
                    std::cout << "ADSBPage: Switched to previous aircraft "
                              << std::hex << selected_icao_ << std::dec << std::endl;
                } else if (current_idx == 0) {
                    // Wrap to last aircraft
                    selected_icao_ = aircraft_list_.back().icao_addr;
                    selected_index_ = aircraft_list_.size() - 1;
                    detail_scroll_offset_ = 0;
                    std::cout << "ADSBPage: Wrapped to last aircraft "
                              << std::hex << selected_icao_ << std::dec << std::endl;
                }
            }
            break;

        case APPKEY_RIGHT:
            // Switch to next aircraft
            if (!aircraft_list_.empty()) {
                // Find current aircraft in list
                int current_idx = -1;
                for (size_t i = 0; i < aircraft_list_.size(); i++) {
                    if (aircraft_list_[i].icao_addr == selected_icao_) {
                        current_idx = i;
                        break;
                    }
                }

                if (current_idx >= 0 && current_idx < (int)aircraft_list_.size() - 1) {
                    // Move to next aircraft
                    selected_icao_ = aircraft_list_[current_idx + 1].icao_addr;
                    selected_index_ = current_idx + 1;
                    detail_scroll_offset_ = 0;  // Reset scroll for new aircraft
                    std::cout << "ADSBPage: Switched to next aircraft "
                              << std::hex << selected_icao_ << std::dec << std::endl;
                } else if (current_idx == (int)aircraft_list_.size() - 1) {
                    // Wrap to first aircraft
                    selected_icao_ = aircraft_list_.front().icao_addr;
                    selected_index_ = 0;
                    detail_scroll_offset_ = 0;
                    std::cout << "ADSBPage: Wrapped to first aircraft "
                              << std::hex << selected_icao_ << std::dec << std::endl;
                }
            }
            break;

        case APPKEY_ENTER:
            // Continue the page cycle: detail -> radar.
            openRadarView();
            break;

        case APPKEY_ESC:
            view_mode_ = VIEW_LIST;
            detail_scroll_offset_ = 0;  // Reset scroll when returning to list
            std::cout << "ADSBPage: Returning to list view" << std::endl;
            break;

        case APPKEY_SCREENSHOT: {
            char filename[64];
            time_t now = time(nullptr);
            struct tm* t = localtime(&now);
            snprintf(filename, sizeof(filename),
                     "/tmp/adsb_%04d%02d%02d_%02d%02d%02d.png",
                     t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec);
            if (fb_.saveScreenshot(filename)) {
                std::cout << "Screenshot saved: " << filename << std::endl;
            } else {
                std::cerr << "Screenshot failed" << std::endl;
            }
            break;
        }

        case APPKEY_L:
            // Open settings menu, return here when done
            settings_return_view_ = VIEW_LIST;
            view_mode_ = VIEW_SETTINGS;
            settings_selected_item_ = 0;
            lat_input_.clear();
            lon_input_.clear();
            std::cout << "ADSBPage: Opening settings menu" << std::endl;
            break;

        default:
            break;
    }
}

void ADSBPage::updateAircraftList() {
    // Convert map to sorted vector for display
    aircraft_list_.clear();
    for (auto& pair : aircraft_db_) {
        aircraft_list_.push_back(pair.second);
    }

    // Sort by last seen (most recent first)
    sortAircraftList();

    // Ensure selected index is valid
    if (selected_index_ >= (int)aircraft_list_.size()) {
        selected_index_ = std::max(0, (int)aircraft_list_.size() - 1);
    }
    if (scroll_offset_ >= (int)aircraft_list_.size()) {
        scroll_offset_ = std::max(0, (int)aircraft_list_.size() - 10);
    }
}

void ADSBPage::removeStaleAircraft() {
    time_t now = time(nullptr);
    auto it = aircraft_db_.begin();
    while (it != aircraft_db_.end()) {
        if (now - it->second.last_seen > 60) {  // 60 second timeout
            it = aircraft_db_.erase(it);
        } else {
            ++it;
        }
    }
}

void ADSBPage::sortAircraftList() {
    // Sort by last seen (most recent first)
    std::sort(aircraft_list_.begin(), aircraft_list_.end(),
              [](const AircraftState& a, const AircraftState& b) {
                  return a.last_seen > b.last_seen;
              });
}

void ADSBPage::calculateDistance(AircraftState& aircraft) {
    if (!observer_position_set_ || !aircraft.position_valid) {
        aircraft.distance = 0.0f;
        aircraft.bearing = 0.0f;
        return;
    }

    // Haversine formula for distance
    const double R = 6371.0;  // Earth radius in km
    double lat1 = observer_lat_ * M_PI / 180.0;
    double lat2 = aircraft.latitude * M_PI / 180.0;
    double dlat = (aircraft.latitude - observer_lat_) * M_PI / 180.0;
    double dlon = (aircraft.longitude - observer_lon_) * M_PI / 180.0;

    double a = sin(dlat/2) * sin(dlat/2) +
               cos(lat1) * cos(lat2) * sin(dlon/2) * sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    aircraft.distance = R * c;

    // Calculate bearing
    double y = sin(dlon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);
    aircraft.bearing = atan2(y, x) * 180.0 / M_PI;
    if (aircraft.bearing < 0) aircraft.bearing += 360.0;
}

void ADSBPage::renderCurrentView() {
    if (help_visible_) {
        if (help_tab_ == 0) {
            ui_.renderKeyHelp();
        } else {
            const char* source = config_.isManualOverride()
                                     ? "MANUAL"
                                     : (gps_.hasFix()
                                            ? "GPS"
                                            : (observer_position_set_ ? "SAVED"
                                                                      : "NOT SET"));
            ui_.renderSettingsOverview(observer_lat_, observer_lon_,
                                       observer_position_set_,
                                       help_setting_index_, source);
        }
        return;
    }

    if (view_mode_ == VIEW_LIST) {
        // Render status bar
        ui_.renderStatusBar(aircraft_list_.size(), total_messages_,
                           message_rate_);

        // Render column headers
        ui_.renderColumnHeaders();

        // Render aircraft list
        ui_.renderAircraftList(aircraft_list_, selected_index_, scroll_offset_);

        // Render statistics
        float max_range = 0.0f;
        for (const auto& ac : aircraft_list_) {
            if (ac.distance > max_range) max_range = ac.distance;
        }
        ui_.renderStatistics(total_messages_, crc_errors_, max_range);

    } else if (view_mode_ == VIEW_DETAIL) {
        // Render detail view - find aircraft by ICAO address
        auto it = aircraft_db_.find(selected_icao_);
        if (it != aircraft_db_.end()) {
            float gain_db = (gain_ >= 0) ? (gain_ / 10.0f) : 35.0f;
            ui_.renderAircraftDetail(it->second,
                                    detail_scroll_offset_,
                                    aircraft_list_.size(),
                                    total_messages_,
                                    message_rate_,
                                    gain_db);
        } else {
            // Aircraft no longer exists, return to list view
            view_mode_ = VIEW_LIST;
            detail_scroll_offset_ = 0;
        }
    } else if (view_mode_ == VIEW_RADAR) {
        // Render radar view
        float max_range = calculateMaxRange();

        // Clear screen
        fb_.clear(COLOR_RADAR_BG);

        // Render status bar
        radar_ui_.renderStatusBar(aircraft_list_.size(), max_range);

        // Render radar display with split layout
        radar_ui_.renderRadarDisplay(aircraft_list_, sweep_angle_, max_range,
                                     observer_lat_, observer_lon_,
                                     selected_index_, scroll_offset_, blink_state_);

    } else if (view_mode_ == VIEW_SETTINGS) {
        // Render settings menu
        renderSettingsMenu();
    }
}

void ADSBPage::generateTestData() {
    // Generate some test aircraft for Phase 1 UI testing
    time_t now = time(nullptr);

    // Test aircraft 1
    AircraftState ac1;
    ac1.icao_addr = 0xA12345;
    strncpy(ac1.callsign, "UAL123", sizeof(ac1.callsign));
    ac1.altitude = 35000;
    ac1.position_valid = true;
    ac1.speed = 450;
    ac1.heading = 270;
    ac1.vertical_rate = 0;
    ac1.velocity_valid = true;
    ac1.last_seen = now - 2;
    ac1.message_count = 145;
    ac1.distance = 12.5f;
    ac1.bearing = 45.0f;
    aircraft_db_[ac1.icao_addr] = ac1;

    // Test aircraft 2
    AircraftState ac2;
    ac2.icao_addr = 0xB67890;
    strncpy(ac2.callsign, "FDX999", sizeof(ac2.callsign));
    ac2.altitude = 38000;
    ac2.position_valid = true;
    ac2.speed = 420;
    ac2.heading = 180;
    ac2.vertical_rate = -500;
    ac2.velocity_valid = true;
    ac2.last_seen = now - 5;
    ac2.message_count = 89;
    ac2.distance = 25.3f;
    ac2.bearing = 120.0f;
    aircraft_db_[ac2.icao_addr] = ac2;

    // Test aircraft 3
    AircraftState ac3;
    ac3.icao_addr = 0xC11111;
    strncpy(ac3.callsign, "SWA456", sizeof(ac3.callsign));
    ac3.altitude = 28000;
    ac3.position_valid = true;
    ac3.speed = 380;
    ac3.heading = 90;
    ac3.vertical_rate = 1200;
    ac3.velocity_valid = true;
    ac3.last_seen = now - 8;
    ac3.message_count = 67;
    ac3.distance = 18.7f;
    ac3.bearing = 200.0f;
    aircraft_db_[ac3.icao_addr] = ac3;

    // Test aircraft 4
    AircraftState ac4;
    ac4.icao_addr = 0xD22222;
    strncpy(ac4.callsign, "DAL789", sizeof(ac4.callsign));
    ac4.altitude = 32000;
    ac4.position_valid = true;
    ac4.speed = 410;
    ac4.heading = 45;
    ac4.vertical_rate = 0;
    ac4.velocity_valid = true;
    ac4.last_seen = now - 12;
    ac4.message_count = 234;
    ac4.distance = 30.2f;
    ac4.bearing = 315.0f;
    aircraft_db_[ac4.icao_addr] = ac4;

    // Test aircraft 5
    AircraftState ac5;
    ac5.icao_addr = 0xE33333;
    strncpy(ac5.callsign, "AAL456", sizeof(ac5.callsign));
    ac5.altitude = 25000;
    ac5.position_valid = true;
    ac5.speed = 350;
    ac5.heading = 315;
    ac5.vertical_rate = -800;
    ac5.velocity_valid = true;
    ac5.last_seen = now - 15;
    ac5.message_count = 156;
    ac5.distance = 22.1f;
    ac5.bearing = 90.0f;
    aircraft_db_[ac5.icao_addr] = ac5;

    // Initialize statistics
    total_messages_ = 691;  // Sum of message counts
    crc_errors_ = 12;
    message_rate_ = 45.0f;
    last_rate_update_ = now;
    messages_since_update_ = 0;
}

void ADSBPage::loadConfig() {
    if (config_.load(config_path_)) {
        observer_lat_ = config_.getObserverLat();
        observer_lon_ = config_.getObserverLon();
        observer_position_set_ = config_.isObserverPositionSet();
        std::cout << "ADSBPage: Loaded observer position: "
                  << observer_lat_ << ", " << observer_lon_ << std::endl;
    } else {
        std::cout << "ADSBPage: No saved observer position" << std::endl;
    }
}

void ADSBPage::saveConfig() {
    config_.setObserverPosition(observer_lat_, observer_lon_);
    if (config_.save(config_path_)) {
        std::cout << "ADSBPage: Saved observer position: "
                  << observer_lat_ << ", " << observer_lon_ << std::endl;
    } else {
        std::cerr << "ADSBPage: Failed to save configuration" << std::endl;
    }
}

void ADSBPage::renderSettingsMenu() {
    fb_.clear(COLOR_BG_DARK);

    // Title
    ui_.renderText(10, 10, "ADSB Settings", COLOR_TEXT_BRIGHT, true);

    // Menu items
    int y = 40;
    const int line_height = 20;

    // Latitude input
    char lat_label[64];
    if (editing_lat_) {
        snprintf(lat_label, sizeof(lat_label), "> Latitude: %s_", lat_input_.c_str());
    } else {
        snprintf(lat_label, sizeof(lat_label), "%s Latitude: %s",
                 (settings_selected_item_ == 0) ? ">" : " ",
                 lat_input_.empty() ? (observer_position_set_ ? std::to_string(observer_lat_).c_str() : "NOT SET") : lat_input_.c_str());
    }
    ui_.renderText(20, y, lat_label,
                   (settings_selected_item_ == 0 || editing_lat_) ? COLOR_TEXT_BRIGHT : COLOR_TEXT,
                   false);
    y += line_height;

    // Longitude input
    char lon_label[64];
    if (editing_lon_) {
        snprintf(lon_label, sizeof(lon_label), "> Longitude: %s_", lon_input_.c_str());
    } else {
        snprintf(lon_label, sizeof(lon_label), "%s Longitude: %s",
                 (settings_selected_item_ == 1) ? ">" : " ",
                 lon_input_.empty() ? (observer_position_set_ ? std::to_string(observer_lon_).c_str() : "NOT SET") : lon_input_.c_str());
    }
    ui_.renderText(20, y, lon_label,
                   (settings_selected_item_ == 1 || editing_lon_) ? COLOR_TEXT_BRIGHT : COLOR_TEXT,
                   false);
    y += line_height;

    y += 10;  // Extra spacing

    // Save button
    ui_.renderText(20, y, (settings_selected_item_ == 2) ? "> Save" : "  Save",
                   (settings_selected_item_ == 2) ? COLOR_TEXT_BRIGHT : COLOR_TEXT,
                   false);
    y += line_height;

    // Cancel button
    ui_.renderText(20, y, (settings_selected_item_ == 3) ? "> Cancel" : "  Cancel",
                   (settings_selected_item_ == 3) ? COLOR_TEXT_BRIGHT : COLOR_TEXT,
                   false);

    // Help text
    ui_.renderText(10, 150, "Enter: Select/Confirm  ESC: Cancel", COLOR_TEXT_DIM, false);
}

namespace {
bool parseLatLon(const std::string& lat_text, const std::string& lon_text,
                 double& lat, double& lon) {
    if (lat_text.empty() || lon_text.empty()) return false;
    try {
        size_t used = 0;
        lat = std::stod(lat_text, &used);
        if (used != lat_text.size()) return false;
        lon = std::stod(lon_text, &used);
        if (used != lon_text.size()) return false;
    } catch (...) {
        return false;
    }
    return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}
}  // namespace

void ADSBPage::handleSettingsInput(AppKeyCode key) {
    // Handle text input when editing
    if (editing_lat_ || editing_lon_) {
        std::string& input = editing_lat_ ? lat_input_ : lon_input_;

        switch (key) {
            case APPKEY_0: input += '0'; break;
            case APPKEY_1: input += '1'; break;
            case APPKEY_2: input += '2'; break;
            case APPKEY_3: input += '3'; break;
            case APPKEY_4: input += '4'; break;
            case APPKEY_5: input += '5'; break;
            case APPKEY_6: input += '6'; break;
            case APPKEY_7: input += '7'; break;
            case APPKEY_8: input += '8'; break;
            case APPKEY_9: input += '9'; break;
            case APPKEY_DOT: input += '.'; break;
            case APPKEY_MINUS:
                if (input.empty() || input[0] != '-') {
                    input = "-" + input;
                }
                break;

            case APPKEY_BACKSPACE:
                if (!input.empty()) {
                    input.pop_back();
                }
                break;

            case APPKEY_ENTER:
                // Finish editing
                editing_lat_ = false;
                editing_lon_ = false;
                break;

            case APPKEY_ESC:
                // Cancel editing
                if (editing_lat_) {
                    lat_input_.clear();
                    editing_lat_ = false;
                } else if (editing_lon_) {
                    lon_input_.clear();
                    editing_lon_ = false;
                }
                break;

            default:
                break;
        }
        return;
    }

    // Handle menu navigation
    switch (key) {
        case APPKEY_UP:
            if (settings_selected_item_ > 0) {
                settings_selected_item_--;
            }
            break;

        case APPKEY_DOWN:
            if (settings_selected_item_ < 3) {
                settings_selected_item_++;
            }
            break;

        case APPKEY_ENTER:
            if (settings_selected_item_ == 0) {
                // Edit latitude
                editing_lat_ = true;
                if (lat_input_.empty() && observer_position_set_) {
                    lat_input_ = std::to_string(observer_lat_);
                }
            } else if (settings_selected_item_ == 1) {
                // Edit longitude
                editing_lon_ = true;
                if (lon_input_.empty() && observer_position_set_) {
                    lon_input_ = std::to_string(observer_lon_);
                }
            } else if (settings_selected_item_ == 2) {
                // Save (ignore incomplete or out-of-range input)
                double lat = 0.0;
                double lon = 0.0;
                if (parseLatLon(lat_input_, lon_input_, lat, lon)) {
                    observer_lat_ = lat;
                    observer_lon_ = lon;
                    observer_position_set_ = true;
                    config_.setManualOverride(true);
                    saveConfig();
                    std::cout << "ADSBPage: Observer position set to "
                              << observer_lat_ << ", " << observer_lon_ << std::endl;
                }
                // Return to the view the editor was opened from
                view_mode_ = settings_return_view_;
                lat_input_.clear();
                lon_input_.clear();
                settings_selected_item_ = 0;
            } else if (settings_selected_item_ == 3) {
                // Cancel
                view_mode_ = VIEW_DETAIL;
                lat_input_.clear();
                lon_input_.clear();
                settings_selected_item_ = 0;
            }
            break;

        case APPKEY_ESC:
            // Cancel and return to the view the editor was opened from
            view_mode_ = settings_return_view_;
            lat_input_.clear();
            lon_input_.clear();
            settings_selected_item_ = 0;
            break;

        default:
            break;
    }
}

void ADSBPage::handleRadarInput(AppKeyCode key) {
    switch (key) {
        case APPKEY_UP:
            if (selected_index_ > 0) {
                selected_index_--;
                beeps_.play(1000.0f, 15, 0.06f);
                // Adjust scroll if needed
                if (selected_index_ < scroll_offset_) {
                    scroll_offset_ = selected_index_;
                }
            }
            break;

        case APPKEY_DOWN:
            if (selected_index_ < (int)aircraft_list_.size() - 1) {
                selected_index_++;
                beeps_.play(1000.0f, 15, 0.06f);
                // Adjust scroll if needed
                int max_visible = 8;  // MAX_VISIBLE_ROWS - 1 (header row)
                if (selected_index_ >= scroll_offset_ + max_visible) {
                    scroll_offset_ = selected_index_ - max_visible + 1;
                }
            }
            break;

        // No PgUp/PgDn on the Cardputer keyboard: Left/Right page the list.
        case APPKEY_LEFT:
            selected_index_ = std::max(0, selected_index_ - 5);
            scroll_offset_ = std::max(0, scroll_offset_ - 5);
            beeps_.play(1320.0f, 15, 0.06f);
            break;

        case APPKEY_RIGHT:
            selected_index_ = std::min((int)aircraft_list_.size() - 1, selected_index_ + 5);
            scroll_offset_ = std::min((int)aircraft_list_.size() - 8, scroll_offset_ + 5);
            if (scroll_offset_ < 0) scroll_offset_ = 0;
            beeps_.play(1320.0f, 15, 0.06f);
            break;

        case APPKEY_ENTER:
        case APPKEY_ESC:
        case APPKEY_R:
            // Enter continues the cycle: radar -> list.
            view_mode_ = VIEW_LIST;
            std::cout << "ADSBPage: Returning to list view from radar" << std::endl;
            break;

        case APPKEY_L:
            // Open settings menu, return to the radar when done
            settings_return_view_ = VIEW_RADAR;
            view_mode_ = VIEW_SETTINGS;
            settings_selected_item_ = 0;
            lat_input_.clear();
            lon_input_.clear();
            std::cout << "ADSBPage: Opening settings menu from radar" << std::endl;
            break;

        case APPKEY_SCREENSHOT: {
            char filename[64];
            time_t now = time(nullptr);
            struct tm* t = localtime(&now);
            snprintf(filename, sizeof(filename),
                     "/tmp/radar_%04d%02d%02d_%02d%02d%02d.png",
                     t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec);
            if (fb_.saveScreenshot(filename)) {
                std::cout << "Screenshot saved: " << filename << std::endl;
            } else {
                std::cerr << "Screenshot failed" << std::endl;
            }
            break;
        }

        case APPKEY_G:
            // Toggle gain mode
            if (gain_ < 0) {
                gain_ = 0;
                sdr_.setAutoGain(false);
                sdr_.setGain(gain_);
                std::cout << "ADSBPage: Manual gain mode (0 dB)" << std::endl;
            } else {
                gain_ = -1;
                sdr_.setAutoGain(true);
                std::cout << "ADSBPage: Auto gain mode" << std::endl;
            }
            break;

        case APPKEY_PLUS:
            if (gain_ >= 0 && gain_ < 490) {
                gain_ += 10;
                sdr_.setGain(gain_);
                std::cout << "ADSBPage: Gain = " << gain_/10 << " dB" << std::endl;
            }
            break;

        case APPKEY_MINUS:
            if (gain_ >= 0 && gain_ > 0) {
                gain_ -= 10;
                sdr_.setGain(gain_);
                std::cout << "ADSBPage: Gain = " << gain_/10 << " dB" << std::endl;
            }
            break;

        default:
            break;
    }
}

void ADSBPage::handleHelpInput(AppKeyCode key) {
    if (key == APPKEY_H) {
        help_visible_ = false;
        return;
    }
    if (key == APPKEY_LEFT || key == APPKEY_RIGHT) {
        help_tab_ = (help_tab_ + 1) % 2;
        help_setting_index_ = 0;
        beeps_.play(1000.0f, 12, 0.05f);
        return;
    }
    if (help_tab_ != 1) {
        return;
    }
    if (key == APPKEY_UP) {
        help_setting_index_ = (help_setting_index_ + 2) % 3;
    } else if (key == APPKEY_DOWN) {
        help_setting_index_ = (help_setting_index_ + 1) % 3;
    } else if (key == APPKEY_ENTER) {
        if (help_setting_index_ <= 1) {
            // Hand over to the existing position editor.
            settings_return_view_ = VIEW_LIST;
            help_visible_ = false;
            view_mode_ = VIEW_SETTINGS;
            settings_selected_item_ = help_setting_index_;
            lat_input_.clear();
            lon_input_.clear();
            if (help_setting_index_ == 0) {
                editing_lat_ = true;
                if (observer_position_set_) {
                    lat_input_ = std::to_string(observer_lat_);
                }
            } else {
                editing_lon_ = true;
                if (observer_position_set_) {
                    lon_input_ = std::to_string(observer_lon_);
                }
            }
        } else {
            observer_lat_ = 0.0;
            observer_lon_ = 0.0;
            observer_position_set_ = false;
            config_.clearObserverPosition();
            config_.setManualOverride(false);
            config_.save(config_path_);
            beeps_.play(880.0f, 40, 0.12f);
            std::cout << "ADSBPage: Observer position cleared" << std::endl;
        }
    }
}

float ADSBPage::calculateMaxRange() {
    // Find furthest aircraft
    float max_dist = 0.0f;
    for (const auto& ac : aircraft_list_) {
        if (ac.distance > max_dist) {
            max_dist = ac.distance;
        }
    }

    // Round up to next 10km increment
    float max_range = ceil(max_dist / 10.0f) * 10.0f;

    // Minimum 10km, maximum 200km
    if (max_range < 10.0f) max_range = 10.0f;
    if (max_range > 200.0f) max_range = 200.0f;

    return max_range;
}
