#include "pages/adsb_page.h"
#include "input_handler.h"
#include "app_config.h"
#include "system_volume.h"
#include "screenshot.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

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

ADSBPage::ADSBPage(Framebuffer& fb, SDRController& sdr, InputHandler& input,
                   ADSBUIRenderer& ui, RadarUIRenderer& radar_ui)
    : fb_(fb), sdr_(sdr), input_(input), ui_(ui), radar_ui_(radar_ui),
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
      imu_enabled_(true),
      heading_up_(true),
      radar_rotation_(0.0f),
      radar_zoom_(1.0f),
      radar_zoom_target_(1.0f),
      radar_zoom_velocity_(0.0f),
      radar_view_age_(0.0f),
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
    map_bg_.loadConfig();

    // Compass: BMI270 + BMM150 on boards that have them. Missing hardware
    // is not an error - the radar simply stays north-up.
    imu_.open();

    const std::string radar_theme = AppConfig::instance().data()
                                        .value("radar", nlohmann::json::object())
                                        .value("theme", "green");
    radar_theme_ = (radar_theme == "red") ? 1 : 0;
    radar_ui_.setTheme(radar_theme_);
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
        // Per-uid output directory: a root-run test or another user must not
        // leave a directory the normal pi session cannot write to.
        readsb_ = std::make_unique<READSBController>(
            readsb_path,
            "/tmp/zerosdr-readsb-" + std::to_string((long)getuid()) + "/"
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

    // The IIO devices may appear only after the page was first created.
    if (!imu_.available()) imu_.open();

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

    // Keep the gyro heading integrated in every ADSB view, not only while
    // the radar is visible; otherwise a rotation made in the list would be
    // lost by the time the scope is opened.
    if (imu_enabled_) imu_.update(delta_time);

    // Update radar sweep angle if in radar view
    if (view_mode_ == VIEW_RADAR) {
        radar_view_age_ += delta_time;
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

        // Compass heading and the smoothed rose rotation.
        updateRadarRotation(delta_time);
        updateRadarZoom(delta_time);

        // Map background: the worker only starts a download when the
        // position or range zoom changes, so this is cheap every frame.
        // Wait a few seconds so the first request already sees the live
        // traffic range instead of fetching a 10 km patch first.
        if (observer_position_set_ && radar_view_age_ >= 3.0f) {
            map_bg_.request(observer_lat_, observer_lon_,
                            calculateMaxRange());
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
            // A stale file means readsb stopped producing samples (USB wedge,
            // unwritable output directory, ...). Do not keep showing frozen
            // numbers: restart readsb occasionally and drop the old table.
            bool feed_fresh = true;
            struct stat st;
            if (!json_path_.empty() &&
                stat(json_path_.c_str(), &st) == 0) {
                const time_t age = now - st.st_mtime;
                feed_fresh = (age <= 5);
                if (!feed_fresh && age > 10) {
                    if (now - last_feed_restart_ >= 30) {
                        std::cout << "ADSBPage: aircraft.json is "
                                  << age << "s old, restarting readsb"
                                  << std::endl;
                        readsb_->stop();
                        const int gain_value =
                            (gain_ >= 0) ? (gain_ / 10) : -1;
                        readsb_->start(gain_value);
                        last_feed_restart_ = now;
                    }
                    aircraft_db_.clear();
                    alt_display_.clear();
                    bank_deg_.clear();
                    prev_heading_.clear();
                    beeps_.stopTone();
                    vario_icao_ = 0;
                }
            }

            if (feed_fresh &&
                json_parser_.parseFile(json_path_, aircraft_db_)) {
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

                // Calculate distance and accumulate total messages. The
                // bank angle is estimated from the track change rate so the
                // PFD bank pointer follows real turns.
                const float parse_dt =
                    (last_parse_time_ > 0 && now > last_parse_time_)
                        ? (float)(now - last_parse_time_)
                        : 1.0f;
                last_parse_time_ = now;

                uint32_t new_total = 0;
                for (auto& pair : aircraft_db_) {
                    calculateDistance(pair.second);
                    updateBankEstimate(pair.second, parse_dt);
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

    // Smooth the altitude shown by the PFD drum. The JSON only updates once
    // per second, so the display eases towards the new value and the last
    // two digits roll in the direction of travel.
    const float alt_alpha = std::min(1.0f, delta_time * 4.0f);
    for (auto& pair : aircraft_db_) {
        AircraftState& ac = pair.second;
        auto it = alt_display_.find(ac.icao_addr);
        if (it == alt_display_.end()) {
            alt_display_[ac.icao_addr] = (float)ac.altitude;
            ac.altitude_display = (float)ac.altitude;
        } else if (ac.position_valid && ac.altitude > 0) {
            float& shown = it->second;
            const float target = (float)ac.altitude;
            if (std::fabs(target - shown) < 0.5f) {
                shown = target;
            } else {
                shown += (target - shown) * alt_alpha;
            }
            ac.altitude_display = shown;
        }
    }

    // Update aircraft list for display
    updateAircraftList();
    updateVarioSound();

    // Long hold on F (the up arrow) jumps straight to the top of the list.
    if (view_mode_ == VIEW_LIST) {
        if (input_.getHoldDuration(APPKEY_UP) >= 600) {
            if (!list_top_jump_) {
                list_top_jump_ = true;
                selected_index_ = 0;
                scroll_offset_ = 0;
                beeps_.play(1568.0f, 30, 0.08f);
                std::cout << "ADSBPage: Long F/up jump to top" << std::endl;
            }
        } else {
            list_top_jump_ = false;
        }
    }

    // Render current view
    renderCurrentView();
    if (!help_visible_) drawVolumeToast();
    fb_.update();
}

void ADSBPage::onShortcut(const char* id) {
    if (id && std::strcmp(id, "radar") == 0) {
        std::cout << "ADSBPage: Radar shortcut" << std::endl;
        openRadarView();
    }
}

void ADSBPage::onVolumeChanged(int percent) {
    if (percent < 0) return;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "VOL %d%%", percent);
    volume_toast_ = buf;
    volume_toast_until_ = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(1500);
    beeps_.play(1200.0f, 20, 0.05f);
}

void ADSBPage::drawVolumeToast() {
    if (volume_toast_.empty()) return;
    if (std::chrono::steady_clock::now() >= volume_toast_until_) return;

    const int text_w = static_cast<int>(volume_toast_.size()) * 9;
    const int w = text_w + 12;
    const int x = (320 - w) / 2;
    const int y = 21;
    fb_.fillRect(x, y, w, 19, COLOR_ADSB_STATUSBAR_BG);
    fb_.drawRect(x, y, w, 19, COLOR_YELLOW);
    ui_.drawText(x + 6, y + 1, volume_toast_.c_str(), COLOR_YELLOW);
}

void ADSBPage::openRadarView() {
    if (observer_position_set_) {
        view_mode_ = VIEW_RADAR;
        sweep_angle_ = 0.0f;
        last_sweep_sector_ = -1;
        radar_view_age_ = 0.0f;
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
                int max_visible = 6;  // MAX_VISIBLE_ROWS from renderer
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
            scroll_offset_ = std::min((int)aircraft_list_.size() - 6, scroll_offset_ + 5);
            if (scroll_offset_ < 0) scroll_offset_ = 0;
            beeps_.play(1320.0f, 15, 0.06f);
            break;

        case APPKEY_ENTER:
            // Enter continues the view cycle: list -> radar -> detail -> list.
            openRadarView();
            break;

        case APPKEY_L:
            // Position editor. It must work from the list too: that is the
            // first view after opening the page, and the README lists L as the
            // position shortcut.
            settings_return_view_ = VIEW_LIST;
            view_mode_ = VIEW_SETTINGS;
            settings_selected_item_ = 0;
            lat_input_.clear();
            lon_input_.clear();
            std::cout << "ADSBPage: Opening settings menu from list" << std::endl;
            break;

        case APPKEY_SCREENSHOT: {
            const std::string path = makeScreenshotPath();
            if (fb_.saveScreenshot(path.c_str())) {
                std::cout << "Screenshot saved: " << path << std::endl;
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

        case APPKEY_M:
            pfd_square_ = !pfd_square_;
            beeps_.play(pfd_square_ ? 1320.0f : 880.0f, 30, 0.08f);
            std::cout << "ADSBPage: PFD shape "
                      << (pfd_square_ ? "rounded square" : "round")
                      << std::endl;
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
            // Continue the view cycle: detail -> list.
            view_mode_ = VIEW_LIST;
            std::cout << "ADSBPage: Returning to list view from detail"
                      << std::endl;
            break;


        case APPKEY_SCREENSHOT: {
            const std::string path = makeScreenshotPath();
            if (fb_.saveScreenshot(path.c_str())) {
                std::cout << "Screenshot saved: " << path << std::endl;
            } else {
                std::cerr << "Screenshot failed" << std::endl;
            }
            break;
        }

        case APPKEY_L:
            // Open the position editor and come back to this aircraft.
            settings_return_view_ = VIEW_DETAIL;
            view_mode_ = VIEW_SETTINGS;
            settings_selected_item_ = 0;
            lat_input_.clear();
            lon_input_.clear();
            std::cout << "ADSBPage: Opening settings menu from detail" << std::endl;
            break;

        default:
            break;
    }
}

void ADSBPage::updateAircraftList() {
    // Keep the cursor on the same aircraft when the order changes: the radar
    // sorts by distance, so one new nearby target shifts every index.
    const bool have_cursor = !aircraft_list_.empty() && selected_index_ >= 0 &&
                             selected_index_ < (int)aircraft_list_.size();
    const uint32_t cursor_icao =
        have_cursor ? aircraft_list_[selected_index_].icao_addr : 0;

    // Convert map to sorted vector for display
    aircraft_list_.clear();
    for (auto& pair : aircraft_db_) {
        aircraft_list_.push_back(pair.second);
    }

    sortAircraftList();

    if (have_cursor) {
        for (size_t i = 0; i < aircraft_list_.size(); ++i) {
            if (aircraft_list_[i].icao_addr == cursor_icao) {
                selected_index_ = (int)i;
                break;
            }
        }
    }

    // Ensure selected index is valid
    if (selected_index_ >= (int)aircraft_list_.size()) {
        selected_index_ = std::max(0, (int)aircraft_list_.size() - 1);
    }
    if (scroll_offset_ >= (int)aircraft_list_.size()) {
        scroll_offset_ = std::max(0, (int)aircraft_list_.size() - 10);
    }

    // Keep the highlighted row inside the visible rows. The radar panel has
    // one row less than the full-screen list because of its header line.
    const int max_visible = (view_mode_ == VIEW_RADAR) ? 8 : 6;
    if (selected_index_ < scroll_offset_) scroll_offset_ = selected_index_;
    if (selected_index_ >= scroll_offset_ + max_visible) {
        scroll_offset_ = selected_index_ - max_visible + 1;
    }
    const int max_scroll = std::max(0, (int)aircraft_list_.size() - max_visible);
    if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll;
    if (scroll_offset_ < 0) scroll_offset_ = 0;
}

void ADSBPage::removeStaleAircraft() {
    time_t now = time(nullptr);
    auto it = aircraft_db_.begin();
    while (it != aircraft_db_.end()) {
        if (now - it->second.last_seen > 60) {  // 60 second timeout
            bank_deg_.erase(it->second.icao_addr);
            prev_heading_.erase(it->second.icao_addr);
            alt_display_.erase(it->second.icao_addr);
            it = aircraft_db_.erase(it);
        } else {
            ++it;
        }
    }
}

void ADSBPage::sortAircraftList() {
    // Nearest first, in every ADSB view: the list, the radar panel and the
    // detail paging all use this order. Aircraft without a position have no
    // distance yet and sink to the bottom, newest first, so they never push a
    // real target off the visible rows.
    std::stable_sort(aircraft_list_.begin(), aircraft_list_.end(),
                     [](const AircraftState& a, const AircraftState& b) {
                         const bool a_pos = a.position_valid && a.distance > 0.0f;
                         const bool b_pos = b.position_valid && b.distance > 0.0f;
                         if (a_pos != b_pos) return a_pos;
                         if (a_pos && b_pos &&
                             std::fabs(a.distance - b.distance) > 0.01f) {
                             return a.distance < b.distance;
                         }
                         return a.last_seen > b.last_seen;
                     });
}

void ADSBPage::updateVarioSound() {
    // Variometer dots only while the altitude readout is actually rolling:
    // the smoothed altitude has to be catching up with a new sample. The
    // audio thread stays alive while the aircraft has altitude and a vertical
    // speed, but the pulse amplitude is zero once the digits settle.
    if (view_mode_ != VIEW_DETAIL || help_visible_) {
        beeps_.stopTone();
        vario_icao_ = 0;
        return;
    }

    auto it = aircraft_db_.find(selected_icao_);
    if (it == aircraft_db_.end() || !it->second.velocity_valid ||
        !it->second.position_valid || it->second.altitude <= 0) {
        // No altitude data means no altitude readout on the PFD; stay silent.
        beeps_.stopTone();
        return;
    }

    const int vs = it->second.vertical_rate;
    const int magnitude = std::abs(vs);
    if (magnitude < 100) {  // level flight is silent
        beeps_.stopTone();
        return;
    }

    auto alt_it = alt_display_.find(selected_icao_);
    if (alt_it == alt_display_.end()) {
        beeps_.stopTone();
        return;
    }

    const float target = (float)it->second.altitude;
    const bool rolling = std::fabs(target - alt_it->second) > 0.5f;

    const float t = std::min(1.0f, (float)(magnitude - 100) / 2900.0f);

    // Dense dots: about 220 ms apart at 100 fpm, 40 ms at 3000+ fpm.
    const int period_ms = (int)std::lround(220.0f - t * 180.0f);
    const int on_ms = 12;

    // Rising pitch for a climb, falling pitch for a descent.
    const float freq = (vs > 0) ? (1000.0f + t * 600.0f)
                                : (800.0f - t * 300.0f);
    const float amp = rolling ? (0.22f + t * 0.06f) : 0.0f;

    beeps_.setPulse(freq, period_ms, on_ms, amp);

    if (vario_icao_ != selected_icao_) {
        vario_icao_ = selected_icao_;
        std::cout << "ADSBPage: Vario " << (vs > 0 ? "climb " : "descent ")
                  << magnitude << " fpm, dots every " << period_ms << " ms"
                  << std::endl;
    }
}

void ADSBPage::updateBankEstimate(AircraftState& aircraft,
                                  float delta_seconds) {
    // Standard coordinated turn: tan(bank) = v * omega / g. The track is
    // reported once per second, so the delta is a coarse but usable turn
    // rate. The estimate is smoothed per ICAO and decays to wings level
    // when the aircraft stops turning (or stops reporting a track).
    float target = 0.0f;
    auto prev = prev_heading_.find(aircraft.icao_addr);
    if (prev != prev_heading_.end() && aircraft.velocity_valid &&
        aircraft.speed > 60 && delta_seconds >= 1.0f) {
        float dh = (float)aircraft.heading - (float)prev->second;
        while (dh > 180.0f) dh -= 360.0f;
        while (dh < -180.0f) dh += 360.0f;

        const float turn_rate = dh / delta_seconds;  // deg/s
        if (std::fabs(turn_rate) >= 0.5f) {
            const float omega = turn_rate * (float)M_PI / 180.0f;
            const float v = (float)aircraft.speed * 0.514444f;  // m/s
            target = std::atan2(v * omega, 9.80665f) * 180.0f / (float)M_PI;
            target = std::max(-45.0f, std::min(45.0f, target));
        }
    }

    prev_heading_[aircraft.icao_addr] = aircraft.heading;

    float& bank = bank_deg_[aircraft.icao_addr];
    bank += (target - bank) * 0.35f;
    if (std::fabs(bank) < 0.3f) bank = 0.0f;
    aircraft.bank = bank;
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
                                    gain_db,
                                    pfd_square_);
        } else {
            // Aircraft no longer exists, return to list view
            view_mode_ = VIEW_LIST;
            detail_scroll_offset_ = 0;
        }
    } else if (view_mode_ == VIEW_RADAR) {
        // Render radar view
        float max_range = calculateMaxRange();

        // Clear screen
        fb_.clear(radar_ui_.backgroundColor());

        // Layer order: full-screen map, status-bar plate, radar scope/list,
        // then the status text. The scope stays above the top bar while the
        // map fills the whole display underneath the panels.
        radar_ui_.drawMapBackground(map_bg_, max_range, radar_zoom_,
                                    radar_rotation_);
        radar_ui_.renderStatusBackground(radar_zoom_);
        radar_ui_.renderRadarDisplay(aircraft_list_, sweep_angle_, max_range,
                                     observer_lat_, observer_lon_,
                                     selected_index_, scroll_offset_,
                                     blink_state_, radar_rotation_, radar_zoom_,
                                     false);

        const bool compass_active = imu_enabled_ && imu_.hasHeading();
        radar_ui_.renderStatusText(aircraft_list_.size(), max_range,
                                   compass_active, imu_.heading(),
                                   heading_up_ && compass_active, radar_zoom_);

        // Small map attribution at the bottom-right of the scope.
        if (map_bg_.hasImage()) {
            const char* attr = map_bg_.attribution();
            int x = 316 - (int)strlen(attr) * 6;
            if (x < 130) x = 130;
            radar_ui_.renderText(x, 161, attr, radar_ui_.textColor());
        }

    } else if (view_mode_ == VIEW_SETTINGS) {
        // Render settings menu
        renderSettingsMenu();
    }

    // Draw the translucent help overlay on top of the live view.
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
                                       help_setting_index_, source,
                                       map_bg_.providerLabel());
        }
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
    loadImuConfig();
}

void ADSBPage::loadImuConfig() {
    const auto& data = AppConfig::instance().data();
    if (!data.contains("imu") || !data["imu"].is_object()) return;
    const auto& imu = data["imu"];
    imu_enabled_ = imu.value("enabled", true);
    heading_up_ = imu.value("heading_up", true);
    imu_.setOffsetDeg(imu.value("offset_deg", 0.0f));
    imu_.setInvert(imu.value("invert", false));
}

void ADSBPage::saveImuConfig() {
    auto& data = AppConfig::instance().data();
    if (!data.contains("imu") || !data["imu"].is_object()) {
        data["imu"] = nlohmann::json::object();
    }
    auto& imu = data["imu"];
    imu["enabled"] = imu_enabled_;
    imu["heading_up"] = heading_up_;
    imu["offset_deg"] = imu_.offsetDeg();
    imu["invert"] = imu_.invert();
    if (!AppConfig::instance().save()) {
        std::cerr << "ADSBPage: Failed to save compass settings" << std::endl;
    }
}

void ADSBPage::updateRadarRotation(float delta_time) {
    // Target rotation for the rose: the compass heading in heading-up mode,
    // north (0) in north-up mode. Easing keeps toggles and sensor jitter
    // from snapping the whole scope around.
    const float target =
        (imu_enabled_ && heading_up_ && imu_.hasHeading()) ? imu_.heading()
                                                          : 0.0f;

    float delta = target - radar_rotation_;
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;

    const float step = std::min(1.0f, delta_time * 6.0f);
    radar_rotation_ += delta * step;
    if (radar_rotation_ < 0.0f) radar_rotation_ += 360.0f;
    if (radar_rotation_ >= 360.0f) radar_rotation_ -= 360.0f;
}

void ADSBPage::updateRadarZoom(float delta_time) {
    // Slightly under-damped spring: the scope overshoots a little and settles.
    // Small fixed sub-steps keep it stable when a render frame runs long on
    // the Pi Zero.
    constexpr float kStiffness = 190.0f;
    constexpr float kDamping = 13.0f;
    constexpr float kMaxStep = 0.01f;

    float remaining = std::max(0.0f, delta_time);
    while (remaining > 0.0f) {
        const float h = std::min(remaining, kMaxStep);
        const float accel = kStiffness * (radar_zoom_target_ - radar_zoom_) -
                            kDamping * radar_zoom_velocity_;
        radar_zoom_velocity_ += accel * h;
        radar_zoom_ += radar_zoom_velocity_ * h;
        remaining -= h;
    }

    if (std::fabs(radar_zoom_target_ - radar_zoom_) < 0.001f &&
        std::fabs(radar_zoom_velocity_) < 0.01f) {
        radar_zoom_ = radar_zoom_target_;
        radar_zoom_velocity_ = 0.0f;
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

void ADSBPage::cycleMapProvider() {
    // Map source: ESRI Dark -> Gaode/AMAP -> OSM -> ESRI Dark.
    auto& root = AppConfig::instance().data();
    if (!root.contains("map") || !root["map"].is_object()) {
        root["map"] = nlohmann::json::object();
    }
    std::string provider = root["map"].value("provider", "esri_dark");
    if (provider == "esri_dark") {
        provider = "amap";
    } else if (provider == "amap") {
        provider = "osm";
    } else {
        provider = "esri_dark";
    }
    root["map"]["provider"] = provider;
    root["map"]["enabled"] = true;
    AppConfig::instance().save();
    map_bg_.reload();
    beeps_.play(1320.0f, 20, 0.07f);
    std::cout << "ADSBPage: Map source -> " << provider << std::endl;
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

    // Map provider. Enter cycles through the supported sources.
    char map_label[64];
    snprintf(map_label, sizeof(map_label), "%s Map source: %s",
             (settings_selected_item_ == 2) ? ">" : " ",
             map_bg_.providerLabel());
    ui_.renderText(20, y, map_label,
                   (settings_selected_item_ == 2) ? COLOR_TEXT_BRIGHT : COLOR_TEXT,
                   false);
    y += line_height;

    y += 10;  // Extra spacing

    // Save button
    ui_.renderText(20, y, (settings_selected_item_ == 3) ? "> Save" : "  Save",
                   (settings_selected_item_ == 3) ? COLOR_TEXT_BRIGHT : COLOR_TEXT,
                   false);
    y += line_height;

    // Cancel button
    ui_.renderText(20, y, (settings_selected_item_ == 4) ? "> Cancel" : "  Cancel",
                   (settings_selected_item_ == 4) ? COLOR_TEXT_BRIGHT : COLOR_TEXT,
                   false);

    // Help text
    ui_.renderText(10, 146, "SYM+R: minus sign", COLOR_TEXT_DIM, false);
    ui_.renderText(10, 158, "ENTER: select/confirm   ESC: cancel",
                   COLOR_TEXT_DIM, false);
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
                // The Cardputer Zero has no dedicated minus key: SYM + R
                // sends KEY_MINUS. Toggle the leading sign so a prefilled
                // value can be flipped in either direction.
                if (!input.empty() && input[0] == '-') {
                    input.erase(0, 1);
                } else {
                    input.insert(input.begin(), '-');
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
            if (settings_selected_item_ < 4) {
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
                cycleMapProvider();
            } else if (settings_selected_item_ == 3) {
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
            } else if (settings_selected_item_ == 4) {
                // Cancel
                view_mode_ = settings_return_view_;
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
            // Continue the view cycle: radar -> detail. Without a target the
            // cycle falls through to the list.
            if (!aircraft_list_.empty() && selected_index_ >= 0 &&
                selected_index_ < (int)aircraft_list_.size()) {
                selected_icao_ = aircraft_list_[selected_index_].icao_addr;
                detail_scroll_offset_ = 0;
                view_mode_ = VIEW_DETAIL;
                std::cout << "ADSBPage: Switching to detail view for aircraft "
                          << std::hex << selected_icao_ << std::dec
                          << std::endl;
            } else {
                view_mode_ = VIEW_LIST;
                std::cout << "ADSBPage: Returning to list view from radar"
                          << std::endl;
            }
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

        case APPKEY_M:
            radar_theme_ = (radar_theme_ + 1) % 2;
            radar_ui_.setTheme(radar_theme_);
            {
                auto& root = AppConfig::instance().data();
                if (!root.contains("radar") || !root["radar"].is_object()) {
                    root["radar"] = nlohmann::json::object();
                }
                root["radar"]["theme"] = radar_theme_ ? "red" : "green";
                AppConfig::instance().save();
            }
            beeps_.play(radar_theme_ ? 1320.0f : 880.0f, 30, 0.08f);
            std::cout << "ADSBPage: Radar theme "
                      << (radar_theme_ ? "red" : "green") << std::endl;
            break;

        case APPKEY_I:
            if (!imu_.available() || !imu_enabled_) {
                beeps_.play(220.0f, 80, 0.10f);
                std::cout << "ADSBPage: IMU compass not available"
                          << std::endl;
                break;
            }
            heading_up_ = !heading_up_;
            saveImuConfig();
            beeps_.play(heading_up_ ? 1320.0f : 880.0f, 30, 0.08f);
            std::cout << "ADSBPage: Radar "
                      << (heading_up_ ? "heading-up" : "north-up")
                      << " (heading " << imu_.heading() << " deg)" << std::endl;
            break;

        case APPKEY_0:
            // Compass calibration: point the top edge north, then zero it.
            if (!imu_.available() || !imu_enabled_ || !imu_.hasHeading()) {
                beeps_.play(220.0f, 80, 0.10f);
                break;
            }
            {
                float offset = std::fmod(imu_.offsetDeg() - imu_.heading(),
                                         360.0f);
                if (offset < 0.0f) offset += 360.0f;
                imu_.setOffsetDeg(offset);
                imu_.resetSmoothing();
                saveImuConfig();
                beeps_.play(1760.0f, 40, 0.08f);
                std::cout << "ADSBPage: Compass zeroed, offset "
                          << offset << " deg" << std::endl;
            }
            break;

        case APPKEY_SCREENSHOT: {
            const std::string path = makeScreenshotPath();
            if (fb_.saveScreenshot(path.c_str())) {
                std::cout << "Screenshot saved: " << path << std::endl;
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

        case APPKEY_T:
        case APPKEY_PLUS:
            // T: grow the scope toward a full-screen background.
            radar_zoom_target_ = std::min(3.2f, radar_zoom_target_ * 1.28f);
            beeps_.play(1568.0f, 15, 0.06f);
            std::cout << "ADSBPage: Radar zoom target "
                      << radar_zoom_target_ << "x" << std::endl;
            break;

        case APPKEY_R:
        case APPKEY_MINUS:
            // R: shrink the scope back toward the right panel.
            radar_zoom_target_ = std::max(0.60f, radar_zoom_target_ / 1.28f);
            beeps_.play(1046.0f, 15, 0.06f);
            std::cout << "ADSBPage: Radar zoom target "
                      << radar_zoom_target_ << "x" << std::endl;
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
        help_setting_index_ = (help_setting_index_ + 3) % 4;
    } else if (key == APPKEY_DOWN) {
        help_setting_index_ = (help_setting_index_ + 1) % 4;
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
        } else if (help_setting_index_ == 2) {
            cycleMapProvider();
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
