#include "pages/aprs_page.h"

#include "app_config.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

AprsPage::AprsPage(Framebuffer& fb, SDRController& sdr, InputHandler& input,
                   AprsUIRenderer& ui)
    : fb_(fb), sdr_(sdr), input_(input), ui_(ui) {
    sample_buffer_.reserve(32768);
}

AprsPage::~AprsPage() {
    stopDecodeThread();
}

bool AprsPage::init() {
    std::cout << "AprsPage: Initializing..." << std::endl;
    loadConfig();
    map_bg_.loadConfig();
    return true;
}

void AprsPage::cleanup() {
    stopDecodeThread();
}

void AprsPage::loadConfig() {
    const auto& data = AppConfig::instance().data();
    if (data.contains("aprs") && data["aprs"].is_object()) {
        const auto& aprs = data["aprs"];
        frequency_ = aprs.value("frequency", 144640000u);
        gain_tenths_ = aprs.value("gain_tenths", 400);
        agc_ = aprs.value("agc", true);
    }
    if (frequency_ < 1000000u || frequency_ > 6000000000u) {
        frequency_ = 144640000;
    }
    gain_tenths_ = std::max(0, std::min(496, gain_tenths_));
}

void AprsPage::saveConfig() {
    auto& data = AppConfig::instance().data();
    if (!data.contains("aprs") || !data["aprs"].is_object()) {
        data["aprs"] = nlohmann::json::object();
    }
    data["aprs"]["frequency"] = frequency_;
    data["aprs"]["gain_tenths"] = gain_tenths_;
    data["aprs"]["agc"] = agc_;
    AppConfig::instance().save();
}

void AprsPage::onEnter() {
    std::cout << "AprsPage: Entering APRS mode" << std::endl;
    loadConfig();

    if (sdr_.isCapturing()) sdr_.stopCapture();
    sdr_.close();

    // readsb (ADSB) and acarsdec (ACARS) may still hold the dongle.
    std::system("pkill -9 readsb 2>/dev/null; pkill -9 acarsdec 2>/dev/null");
    usleep(400000);

    if (!sdr_.init(0)) {
        ui_.renderDialog("APRS", "RTL-SDR not available", "ESC / TAB");
        fb_.update();
        sdr_ready_ = false;
        return;
    }

    sdr_.setFrequency(frequency_);
    sdr_.setSampleRate(1200000);
    sdr_.setAGCMode(agc_);
    if (!agc_) sdr_.setGain(gain_tenths_);

    if (!sdr_.startCapture()) {
        ui_.renderDialog("APRS", "Cannot start SDR capture", "ESC / TAB");
        fb_.update();
        sdr_.close();
        sdr_ready_ = false;
        return;
    }
    sdr_.flushBuffer();

    aprs::Decoder::Config config;
    config.sample_rate = 1200000.0;
    config.audio_rate = 48000;
    if (!decoder_.init(config)) {
        ui_.renderDialog("APRS", "Decoder init failed", "ESC / TAB");
        fb_.update();
        sdr_.stopCapture();
        sdr_.close();
        sdr_ready_ = false;
        return;
    }

    decoder_.setPacketCallback([this](const aprs::Packet& packet) {
        store_.apply(packet);
        std::cout << "APRS: " << packet.source << " " << packet.info
                  << std::endl;
    });

    store_.clear();
    loadStation();
    map_bg_.loadConfig();
    if (store_.hasStation()) {
        map_bg_.request(store_.stationLatitude(), store_.stationLongitude(),
                        map_range_nm_ * 1.852f);
    }

    view_ = View::List;
    selected_index_ = 0;
    selected_callsign_.clear();
    scroll_offset_ = 0;
    console_scroll_ = 0;
    help_visible_ = false;
    map_range_nm_ = 25.0f;
    packets_per_minute_ = 0.0f;
    rate_history_.clear();
    sdr_ready_ = true;

    std::cout << "AprsPage: " << frequency_ << " Hz, 1.2 Msps, "
              << (agc_ ? "AGC" : "manual gain") << std::endl;

    startDecodeThread();
    render();
}

void AprsPage::onExit() {
    std::cout << "AprsPage: Exiting APRS mode" << std::endl;
    stopDecodeThread();
    decoder_.setPacketCallback(nullptr);

    if (sdr_.isCapturing()) sdr_.stopCapture();
    sdr_.close();
    sdr_ready_ = false;
}

void AprsPage::startDecodeThread() {
    if (running_) return;
    running_ = true;
    decode_thread_ = std::thread([this] {
        std::vector<uint8_t> buffer;
        buffer.reserve(32768);
        while (running_) {
            if (sdr_.getLatestSamples(buffer, 32768)) {
                decoder_.process(buffer.data(), buffer.size() / 2);
            } else {
                usleep(2000);
            }
        }
    });
}

void AprsPage::stopDecodeThread() {
    running_ = false;
    if (decode_thread_.joinable()) decode_thread_.join();
}

void AprsPage::loadStation() {
    const auto& data = AppConfig::instance().data();
    if (!data.contains("adsb")) return;
    const auto& adsb = data["adsb"];
    const double latitude = adsb.value("observer_latitude", 0.0);
    const double longitude = adsb.value("observer_longitude", 0.0);
    if (latitude != 0.0 || longitude != 0.0) {
        store_.setStation(latitude, longitude, true);
    }
}

void AprsPage::updateRate() {
    const time_t now = time(nullptr);
    const uint64_t total = store_.totalPackets();
    if (rate_history_.empty() || now != rate_history_.back().first) {
        rate_history_.emplace_back(now, total);
    } else {
        rate_history_.back().second = total;
    }
    while (rate_history_.size() > 2 &&
           now - rate_history_.front().first > 60) {
        rate_history_.pop_front();
    }
    const auto& first = rate_history_.front();
    const double seconds = static_cast<double>(now - first.first);
    if (seconds >= 5.0) {
        packets_per_minute_ = static_cast<float>(
            (total - first.second) * 60.0 / seconds);
    } else {
        packets_per_minute_ = 0.0f;
    }
}

void AprsPage::update(float delta_time) {
    if (!sdr_ready_) return;

    // Held arrow keys repeat and accelerate in the scrolling views.
    if ((view_ == View::List || view_ == View::Console) &&
        held_vertical_key_ != APPKEY_NONE) {
        const int hold_ms = input_.getHoldDuration(held_vertical_key_);
        if (hold_ms >= 450) {
            vertical_repeat_accum_ += delta_time;
            const float interval = hold_ms >= 1800 ? 0.045f
                                  : (hold_ms >= 900 ? 0.080f : 0.130f);
            int guard = 0;
            while (vertical_repeat_accum_ >= interval && guard++ < 8) {
                vertical_repeat_accum_ -= interval;
                stepVertical(held_vertical_key_ == APPKEY_UP);
            }
        }
    }

    if (view_ == View::Map && held_horizontal_key_ != APPKEY_NONE) {
        const int hold_ms = input_.getHoldDuration(held_horizontal_key_);
        if (hold_ms >= 450) {
            horizontal_repeat_accum_ += delta_time;
            const float interval = hold_ms >= 1800 ? 0.045f
                                  : (hold_ms >= 900 ? 0.080f : 0.130f);
            int guard = 0;
            while (horizontal_repeat_accum_ >= interval && guard++ < 8) {
                horizontal_repeat_accum_ -= interval;
                selectStation(held_horizontal_key_ == APPKEY_LEFT ? -1 : 1,
                              true);
            }
        }
    }

    if (view_ == View::Detail && held_horizontal_key_ != APPKEY_NONE) {
        const int hold_ms = input_.getHoldDuration(held_horizontal_key_);
        if (hold_ms >= 450) {
            horizontal_repeat_accum_ += delta_time;
            const float interval = hold_ms >= 1800 ? 0.045f
                                  : (hold_ms >= 900 ? 0.080f : 0.130f);
            int guard = 0;
            while (horizontal_repeat_accum_ >= interval && guard++ < 8) {
                horizontal_repeat_accum_ -= interval;
                selectStation(held_horizontal_key_ == APPKEY_LEFT ? -1 : 1,
                              false);
            }
        }
    }

    gps_.poll();
    if (gps_.hasFix()) {
        store_.setStation(gps_.latitude(), gps_.longitude(), true);
    }

    if (store_.hasStation()) {
        map_bg_.request(store_.stationLatitude(), store_.stationLongitude(),
                        map_range_nm_ * 1.852f);
    }

    updateRate();
    render();
}

int AprsPage::selectedIndexIn(
    const std::vector<aprs::StationState>& stations) const {
    if (stations.empty()) return 0;
    if (!selected_callsign_.empty()) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (stations[i].callsign == selected_callsign_) {
                return static_cast<int>(i);
            }
        }
    }
    return std::max(0, std::min(selected_index_,
                                 static_cast<int>(stations.size()) - 1));
}

void AprsPage::stepVertical(bool up) {
    if (view_ == View::List) {
        const auto stations = store_.snapshot();
        const int count = static_cast<int>(stations.size());
        if (count == 0) {
            selected_index_ = 0;
            selected_callsign_.clear();
            return;
        }
        int index = selectedIndexIn(stations);
        if (up) {
            if (index > 0) --index;
        } else if (index + 1 < count) {
            ++index;
        }
        selected_index_ = index;
        selected_callsign_ = stations[static_cast<size_t>(index)].callsign;
    } else if (view_ == View::Detail) {
        selectStation(up ? -1 : 1, false);
    } else if (view_ == View::Console) {
        if (up) {
            ++console_scroll_;
        } else if (console_scroll_ > 0) {
            --console_scroll_;
        }
    }
}

void AprsPage::selectStation(int direction, bool positioned_only) {
    const auto stations = store_.snapshot();
    const int count = static_cast<int>(stations.size());
    if (count == 0) return;
    int index = selectedIndexIn(stations);
    for (int step = 0; step < count; ++step) {
        index = (index + direction + count) % count;
        if (!positioned_only ||
            (stations[static_cast<size_t>(index)].has_position &&
             stations[static_cast<size_t>(index)].distance_nm >= 0.0f)) {
            selected_index_ = index;
            selected_callsign_ =
                stations[static_cast<size_t>(index)].callsign;
            return;
        }
    }
}

void AprsPage::render() {
    const auto stations = store_.snapshot();
    const uint64_t total = store_.totalPackets();
    const int count = static_cast<int>(stations.size());

    // Keep the selected station stable while the list re-sorts by distance.
    if (count == 0) {
        selected_index_ = 0;
        selected_callsign_.clear();
    } else if (selected_callsign_.empty()) {
        selected_index_ = std::max(0, std::min(selected_index_, count - 1));
        selected_callsign_ = stations[static_cast<size_t>(selected_index_)].callsign;
    } else {
        int found = -1;
        for (int i = 0; i < count; ++i) {
            if (stations[static_cast<size_t>(i)].callsign ==
                selected_callsign_) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            selected_index_ = found;
        } else {
            selected_index_ = std::max(0, std::min(selected_index_, count - 1));
            selected_callsign_ =
                stations[static_cast<size_t>(selected_index_)].callsign;
        }
    }

    if (view_ == View::List) {
        if (count == 0) {
            scroll_offset_ = 0;
        } else {
            if (selected_index_ < scroll_offset_) {
                scroll_offset_ = selected_index_;
            } else if (selected_index_ >= scroll_offset_ + 6) {
                scroll_offset_ = selected_index_ - 5;
            }
            scroll_offset_ = std::max(0, std::min(scroll_offset_, count - 1));
        }
        ui_.renderList(stations, selected_index_, scroll_offset_, total,
                       packets_per_minute_, gain_tenths_, agc_, frequency_);
    } else if (view_ == View::Map) {
        if (count > 0 &&
            (!stations[static_cast<size_t>(selected_index_)].has_position ||
             stations[static_cast<size_t>(selected_index_)].distance_nm <
                 0.0f)) {
            for (int i = 0; i < count; ++i) {
                if (stations[static_cast<size_t>(i)].has_position &&
                    stations[static_cast<size_t>(i)].distance_nm >= 0.0f) {
                    selected_index_ = i;
                    selected_callsign_ =
                        stations[static_cast<size_t>(i)].callsign;
                    break;
                }
            }
        }
        ui_.renderMap(stations, selected_index_, map_range_nm_,
                      store_.hasStation(), store_.stationLatitude(),
                      store_.stationLongitude(), total, packets_per_minute_,
                      gain_tenths_, agc_, frequency_, &map_bg_);
    } else if (view_ == View::Detail) {
        const aprs::StationState* selected =
            count > 0 ? &stations[static_cast<size_t>(selected_index_)]
                      : nullptr;
        ui_.renderDetail(selected, stations.size(), total,
                         packets_per_minute_, gain_tenths_, agc_, frequency_);
    } else {
        const auto entries = store_.logSnapshot();
        const int max_scroll = std::max(0, static_cast<int>(entries.size()) - 1);
        console_scroll_ = std::max(0, std::min(console_scroll_, max_scroll));
        ui_.renderConsole(entries, console_scroll_, total, packets_per_minute_,
                          gain_tenths_, agc_, store_.stationCount(),
                          frequency_);
    }

    if (help_visible_) ui_.renderHelp();
    fb_.update();
}

bool AprsPage::handleInput(AppKeyCode key) {
    if (!sdr_ready_) return false;

    if (key != APPKEY_UP && key != APPKEY_DOWN) {
        held_vertical_key_ = APPKEY_NONE;
        vertical_repeat_accum_ = 0.0f;
    }
    if (key != APPKEY_LEFT && key != APPKEY_RIGHT) {
        held_horizontal_key_ = APPKEY_NONE;
        horizontal_repeat_accum_ = 0.0f;
    }

    if (key == APPKEY_H) {
        help_visible_ = !help_visible_;
        return true;
    }
    if (help_visible_) {
        if (key == APPKEY_ESC) help_visible_ = false;
        return true;
    }

    switch (key) {
        case APPKEY_ENTER:
            if (view_ == View::List) {
                view_ = View::Map;
            } else if (view_ == View::Map) {
                view_ = View::Detail;
            } else if (view_ == View::Detail) {
                view_ = View::Console;
                console_scroll_ = 0;
            } else {
                view_ = View::List;
            }
            return true;

        case APPKEY_UP:
        case APPKEY_DOWN: {
            const bool up = (key == APPKEY_UP);
            if (view_ == View::Map) {
                map_range_nm_ = up ? std::max(1.0f, map_range_nm_ / 2.0f)
                                   : std::min(200.0f, map_range_nm_ * 2.0f);
                held_vertical_key_ = APPKEY_NONE;
                vertical_repeat_accum_ = 0.0f;
            } else {
                stepVertical(up);
                held_vertical_key_ = key;
                vertical_repeat_accum_ = 0.0f;
            }
            return true;
        }

        case APPKEY_LEFT:
            if (view_ == View::Map) {
                selectStation(-1, true);
                held_horizontal_key_ = key;
                horizontal_repeat_accum_ = 0.0f;
                return true;
            }
            if (view_ == View::Detail) {
                selectStation(-1, false);
                held_horizontal_key_ = key;
                horizontal_repeat_accum_ = 0.0f;
                return true;
            }
            return false;

        case APPKEY_RIGHT:
            if (view_ == View::Map) {
                selectStation(1, true);
                held_horizontal_key_ = key;
                horizontal_repeat_accum_ = 0.0f;
                return true;
            }
            if (view_ == View::Detail) {
                selectStation(1, false);
                held_horizontal_key_ = key;
                horizontal_repeat_accum_ = 0.0f;
                return true;
            }
            return false;

        case APPKEY_PLUS:
            agc_ = false;
            gain_tenths_ = std::min(496, gain_tenths_ + 10);
            sdr_.setAGCMode(false);
            sdr_.setGain(gain_tenths_);
            saveConfig();
            return true;

        case APPKEY_MINUS:
            agc_ = false;
            gain_tenths_ = std::max(0, gain_tenths_ - 10);
            sdr_.setAGCMode(false);
            sdr_.setGain(gain_tenths_);
            saveConfig();
            return true;

        case APPKEY_G:
            agc_ = !agc_;
            sdr_.setAGCMode(agc_);
            if (!agc_) sdr_.setGain(gain_tenths_);
            saveConfig();
            return true;

        case APPKEY_T: {
            // Cycle the three common APRS frequencies. The station table is
            // per frequency, so start clean.
            stopDecodeThread();
            if (frequency_ == 144640000u) {
                frequency_ = 144800000u;
            } else if (frequency_ == 144800000u) {
                frequency_ = 144390000u;
            } else {
                frequency_ = 144640000u;
            }
            sdr_.setFrequency(frequency_);
            sdr_.flushBuffer();
            decoder_.reset();
            store_.clear();
            selected_index_ = 0;
            selected_callsign_.clear();
            scroll_offset_ = 0;
            console_scroll_ = 0;
            packets_per_minute_ = 0.0f;
            rate_history_.clear();
            saveConfig();
            startDecodeThread();
            std::cout << "AprsPage: frequency " << frequency_ << std::endl;
            return true;
        }

        case APPKEY_ESC:
            if (view_ != View::List) {
                view_ = View::List;
                return true;
            }
            return false;

        default:
            return false;
    }
}

void AprsPage::onVolumeChanged(int percent) {
    (void)percent;
}
