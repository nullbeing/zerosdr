#include "pages/ais_page.h"

#include "app_config.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

AisPage::AisPage(Framebuffer& fb, SDRController& sdr, InputHandler& input,
                 AisUIRenderer& ui)
    : fb_(fb), sdr_(sdr), input_(input), ui_(ui) {
    sample_buffer_.reserve(32768);
}

AisPage::~AisPage() {
    stopDecodeThread();
}

bool AisPage::init() {
    std::cout << "AisPage: Initializing..." << std::endl;
    map_bg_.loadConfig();
    return true;
}

void AisPage::cleanup() {
    stopDecodeThread();
}

void AisPage::onEnter() {
    std::cout << "AisPage: Entering AIS mode" << std::endl;

    if (sdr_.isCapturing()) sdr_.stopCapture();
    sdr_.close();

    // readsb may still hold the dongle after the ADSB page.
    std::system("pkill -9 readsb 2>/dev/null");
    usleep(400000);

    if (!sdr_.init(0)) {
        ui_.renderDialog("AIS", "RTL-SDR not available", "ESC / TAB");
        fb_.update();
        sdr_ready_ = false;
        return;
    }

    sdr_.setFrequency(162000000);
    sdr_.setSampleRate(1920000);
    sdr_.setAGCMode(agc_);
    if (!agc_) sdr_.setGain(gain_tenths_);

    if (!sdr_.startCapture()) {
        ui_.renderDialog("AIS", "Cannot start SDR capture", "ESC / TAB");
        fb_.update();
        sdr_.close();
        sdr_ready_ = false;
        return;
    }
    sdr_.flushBuffer();

    ais::AisDecoder::Config config;
    config.sample_rate = 1920000.0;
    config.stage2_cutoff_hz = 7000.0;
    config.discriminator_cutoff_hz = 6800.0;
    if (!decoder_.init(config)) {
        ui_.renderDialog("AIS", "Decoder init failed", "ESC / TAB");
        fb_.update();
        sdr_.stopCapture();
        sdr_.close();
        sdr_ready_ = false;
        return;
    }

    const auto& data = AppConfig::instance().data();
    bool udp_export = false;
    if (data.contains("ais")) {
        const auto& ais = data["ais"];
        udp_export = ais.value("udp_export", false);
        udp_host_ = ais.value("udp_host", "255.255.255.255");
        udp_port_ = ais.value("udp_port", 10110);
    }
    udp_ready_ = nmea_udp_.open(udp_host_, udp_port_);
    udp_enabled_ = udp_export && udp_ready_;
    ui_.setUdpEnabled(udp_enabled_);
    if (!udp_ready_) {
        std::cerr << "AisPage: UDP NMEA socket unavailable" << std::endl;
    }

    decoder_.setFrameCallback([this](const ais::AisFrame& frame) {
        store_.apply(frame);
        if (udp_enabled_ && udp_ready_ &&
            !nmea_encoder_.isDuplicate(frame)) {
            const auto sentences = nmea_encoder_.encode(frame);
            for (const auto& sentence : sentences) nmea_udp_.send(sentence);
        }
    });
    store_.clear();
    loadStation();
    map_bg_.loadConfig();
    if (store_.hasStation()) {
        map_bg_.request(store_.stationLatitude(), store_.stationLongitude(),
                        plot_range_nm_ * 1.852f);
    }

    view_ = View::List;
    selected_index_ = 0;
    scroll_offset_ = 0;
    console_scroll_ = 0;
    help_visible_ = false;
    plot_range_nm_ = 2.0f;
    messages_per_second_ = 0.0f;
    last_rate_messages_ = 0;
    last_rate_time_ = 0;
    sdr_ready_ = true;

    startDecodeThread();
    render();
}

void AisPage::onExit() {
    std::cout << "AisPage: Exiting AIS mode" << std::endl;
    stopDecodeThread();
    decoder_.setFrameCallback(nullptr);
    beeps_.close();
    nmea_udp_.close();
    udp_ready_ = false;
    udp_enabled_ = false;
    ui_.setUdpEnabled(false);

    if (sdr_.isCapturing()) sdr_.stopCapture();
    sdr_.close();
    sdr_ready_ = false;
}

void AisPage::startDecodeThread() {
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

void AisPage::stopDecodeThread() {
    running_ = false;
    if (decode_thread_.joinable()) decode_thread_.join();
}

void AisPage::loadStation() {
    const auto& data = AppConfig::instance().data();
    if (!data.contains("adsb")) return;
    const auto& adsb = data["adsb"];
    const double latitude = adsb.value("observer_latitude", 0.0);
    const double longitude = adsb.value("observer_longitude", 0.0);
    if (latitude != 0.0 || longitude != 0.0) {
        store_.setStation(latitude, longitude, true);
    }
}

void AisPage::updateRate() {
    const time_t now = time(nullptr);
    const uint64_t total = store_.totalMessages();
    if (last_rate_time_ == 0) {
        last_rate_time_ = now;
        last_rate_messages_ = total;
        return;
    }
    if (now != last_rate_time_) {
        const double seconds = static_cast<double>(now - last_rate_time_);
        const float instant =
            static_cast<float>((total - last_rate_messages_) / seconds);
        messages_per_second_ = 0.6f * messages_per_second_ + 0.4f * instant;
        last_rate_time_ = now;
        last_rate_messages_ = total;
    }
}

void AisPage::update(float delta_time) {
    if (!sdr_ready_) return;

    // Holding Up/Down in the list or console keeps scrolling and
    // accelerates: 130 ms, then 80 ms, then 45 ms.
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

    if ((view_ == View::Plot || view_ == View::Dossier) &&
        held_horizontal_key_ != APPKEY_NONE) {
        const int hold_ms = input_.getHoldDuration(held_horizontal_key_);
        if (hold_ms >= 450) {
            horizontal_repeat_accum_ += delta_time;
            const float interval = hold_ms >= 1800 ? 0.045f
                                  : (hold_ms >= 900 ? 0.080f : 0.130f);
            int guard = 0;
            while (horizontal_repeat_accum_ >= interval && guard++ < 8) {
                horizontal_repeat_accum_ -= interval;
                const int delta =
                    held_horizontal_key_ == APPKEY_LEFT ? -1 : 1;
                if (view_ == View::Plot) {
                    selectTarget(delta);
                } else {
                    selectTargetAny(delta);
                }
            }
        }
    }

    gps_.poll();
    if (gps_.hasFix()) {
        store_.setStation(gps_.latitude(), gps_.longitude(), true);
    }

    if (store_.hasStation()) {
        map_bg_.request(store_.stationLatitude(), store_.stationLongitude(),
                        plot_range_nm_ * 1.852f);
    }

    updateRate();

    const time_t now = time(nullptr);

    if (now != last_alarm_check_) {
        last_alarm_check_ = now;
        bool alarm = false;
        for (const auto& vessel : store_.snapshot()) {
            if (vessel.stale || vessel.cpa_nm < 0.0f ||
                vessel.tcpa_min < 0.0f) {
                continue;
            }
            if (vessel.cpa_nm < 0.5f && vessel.tcpa_min < 10.0f) {
                alarm = true;
                break;
            }
        }
        if (alarm && (!alarm_active_ || now - last_alarm_beep_ >= 5)) {
            beeps_.play(1320.0f, 80, 0.08f);
            last_alarm_beep_ = now;
        }
        alarm_active_ = alarm;
    }

    render();
}

void AisPage::stepVertical(bool up) {
    if (view_ == View::List) {
        const int count = static_cast<int>(store_.vesselCount());
        if (up) {
            if (selected_index_ > 0) --selected_index_;
        } else if (selected_index_ + 1 < count) {
            ++selected_index_;
        }
    } else if (view_ == View::Console) {
        if (up) {
            ++console_scroll_;
        } else if (console_scroll_ > 0) {
            --console_scroll_;
        }
    }
}

void AisPage::selectTarget(int direction) {
    const auto vessels = store_.snapshot();
    const int count = static_cast<int>(vessels.size());
    if (count == 0) return;
    int index = selected_index_;
    for (int step = 0; step < count; ++step) {
        index = (index + direction + count) % count;
        if (vessels[static_cast<size_t>(index)].has_position) {
            selected_index_ = index;
            return;
        }
    }
}

void AisPage::selectTargetAny(int direction) {
    const int count = static_cast<int>(store_.vesselCount());
    if (count == 0) return;
    selected_index_ = (selected_index_ + direction + count) % count;
}

void AisPage::render() {
    const auto vessels = store_.snapshot();
    const uint64_t total = store_.totalMessages();

    if (view_ == View::List) {
        const int count = static_cast<int>(vessels.size());
        if (count == 0) {
            selected_index_ = 0;
            scroll_offset_ = 0;
        } else {
            selected_index_ = std::max(0, std::min(selected_index_, count - 1));
            if (selected_index_ < scroll_offset_) {
                scroll_offset_ = selected_index_;
            } else if (selected_index_ >= scroll_offset_ + 6) {
                scroll_offset_ = selected_index_ - 5;
            }
            scroll_offset_ = std::max(0, std::min(scroll_offset_, count - 1));
        }
        ui_.renderList(vessels, selected_index_, scroll_offset_, total,
                       messages_per_second_, gain_tenths_, agc_);
    } else if (view_ == View::Plot) {
        const int count = static_cast<int>(vessels.size());
        if (count == 0) {
            selected_index_ = 0;
        } else {
            selected_index_ = std::max(0, std::min(selected_index_, count - 1));
            if (!vessels[static_cast<size_t>(selected_index_)].has_position) {
                for (int i = 0; i < count; ++i) {
                    if (vessels[static_cast<size_t>(i)].has_position) {
                        selected_index_ = i;
                        break;
                    }
                }
            }
        }
        ui_.renderPlot(vessels, selected_index_, plot_range_nm_,
                       store_.hasStation(), store_.stationLatitude(),
                       store_.stationLongitude(), total, messages_per_second_,
                       gain_tenths_, agc_, &map_bg_);
    } else if (view_ == View::Dossier) {
        const int count = static_cast<int>(vessels.size());
        const ais::VesselState* selected = nullptr;
        if (count > 0) {
            selected_index_ = std::max(0, std::min(selected_index_, count - 1));
            selected = &vessels[static_cast<size_t>(selected_index_)];
        }
        ui_.renderDossier(selected, vessels.size(), total, messages_per_second_,
                          gain_tenths_, agc_);
    } else {
        const auto entries = store_.logSnapshot();
        const int max_scroll = std::max(0, static_cast<int>(entries.size()) - 1);
        console_scroll_ = std::max(0, std::min(console_scroll_, max_scroll));
        ui_.renderConsole(entries, console_scroll_, total, messages_per_second_,
                          gain_tenths_, agc_, store_.vesselCount());
    }

    if (help_visible_) ui_.renderHelp();
    fb_.update();
}

bool AisPage::handleInput(AppKeyCode key) {
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
                view_ = View::Plot;
            } else if (view_ == View::Plot) {
                view_ = View::Dossier;
            } else if (view_ == View::Dossier) {
                view_ = View::Console;
                console_scroll_ = 0;
            } else {
                view_ = View::List;
            }
            return true;

        case APPKEY_UP:
        case APPKEY_DOWN: {
            const bool up = (key == APPKEY_UP);
            if (view_ == View::Plot) {
                plot_range_nm_ = up ? std::max(0.5f, plot_range_nm_ / 2.0f)
                                    : std::min(32.0f, plot_range_nm_ * 2.0f);
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
            if (view_ == View::Plot) {
                selectTarget(-1);
                held_horizontal_key_ = key;
                horizontal_repeat_accum_ = 0.0f;
                return true;
            }
            if (view_ == View::Dossier) {
                selectTargetAny(-1);
                held_horizontal_key_ = key;
                horizontal_repeat_accum_ = 0.0f;
                return true;
            }
            return false;

        case APPKEY_RIGHT:
            if (view_ == View::Plot) {
                selectTarget(1);
                held_horizontal_key_ = key;
                horizontal_repeat_accum_ = 0.0f;
                return true;
            }
            if (view_ == View::Dossier) {
                selectTargetAny(1);
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
            return true;

        case APPKEY_MINUS:
            agc_ = false;
            gain_tenths_ = std::max(0, gain_tenths_ - 10);
            sdr_.setAGCMode(false);
            sdr_.setGain(gain_tenths_);
            return true;

        case APPKEY_U:
            if (!udp_ready_) {
                udp_ready_ = nmea_udp_.open(udp_host_, udp_port_);
            }
            if (udp_ready_) {
                udp_enabled_ = !udp_enabled_;
                ui_.setUdpEnabled(udp_enabled_);
                auto& config = AppConfig::instance().data();
                if (!config.contains("ais")) {
                    config["ais"] = nlohmann::json::object();
                }
                config["ais"]["udp_export"] = udp_enabled_.load();
                AppConfig::instance().save();
                std::cout << "AisPage: NMEA UDP "
                          << (udp_enabled_ ? "ON" : "OFF") << " "
                          << udp_host_ << ":" << udp_port_ << std::endl;
            } else {
                std::cerr << "AisPage: UDP NMEA socket unavailable"
                          << std::endl;
            }
            return true;

        case APPKEY_G:
            agc_ = !agc_;
            sdr_.setAGCMode(agc_);
            if (!agc_) sdr_.setGain(gain_tenths_);
            return true;

        case APPKEY_ESC:
            if (view_ == View::Console || view_ == View::Plot ||
                view_ == View::Dossier) {
                view_ = View::List;
                return true;
            }
            return false;


        default:
            return false;
    }
}

void AisPage::onVolumeChanged(int percent) {
    (void)percent;
}
