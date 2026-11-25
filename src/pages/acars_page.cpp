#include "pages/acars_page.h"

#include "app_config.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

AcarsPage::AcarsPage(Framebuffer& fb, SDRController& sdr,
                     InputHandler& input, AcarsUIRenderer& ui)
    : fb_(fb), sdr_(sdr), input_(input), ui_(ui) {}

AcarsPage::~AcarsPage() {
    controller_.stop();
}

bool AcarsPage::init() {
    std::cout << "AcarsPage: Initializing..." << std::endl;
    loadConfig();
    return true;
}

void AcarsPage::cleanup() {
    controller_.stop();
}

void AcarsPage::loadConfig() {
    channels_.clear();
    gain_db_ = 40.0f;
    udp_port_ = 5556;

    const auto& data = AppConfig::instance().data();
    if (data.contains("acars")) {
        const auto& acars = data["acars"];
        if (acars.contains("channels") && acars["channels"].is_array()) {
            for (const auto& value : acars["channels"]) {
                if (value.is_number()) channels_.push_back(value.get<float>());
            }
        }
        gain_db_ = acars.value("gain", gain_db_);
        udp_port_ = acars.value("udp_port", udp_port_);
    }
    if (channels_.empty()) {
        channels_ = {131.550f, 131.725f, 131.825f};
    }
}

void AcarsPage::startDecoder() {
    if (!controller_.start(channels_, gain_db_, udp_port_)) {
        ui_.renderDialog("ACARS", controller_.lastError().c_str(),
                         "R RETRY / TAB");
        fb_.update();
    }
}

void AcarsPage::restartDecoder() {
    controller_.stop();
    usleep(250000);
    startDecoder();
}

int AcarsPage::indexOfSeq(const std::vector<acars::Message>& messages,
                          uint64_t seq) const {
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].seq == seq) return static_cast<int>(i);
    }
    return -1;
}

void AcarsPage::stepDetail(const std::vector<acars::Message>& messages,
                           int delta) {
    const int count = static_cast<int>(messages.size());
    if (count == 0) return;
    int index = indexOfSeq(messages, detail_seq_);
    if (index < 0) {
        index = std::max(0, std::min(selected_index_, count - 1));
    }
    index = (index + delta) % count;
    if (index < 0) index += count;
    selected_index_ = index;
    detail_seq_ = messages[static_cast<size_t>(index)].seq;
    detail_scroll_ = 0;
}

void AcarsPage::onEnter() {
    std::cout << "AcarsPage: Entering ACARS mode" << std::endl;

    // ACARS needs the dongle exclusively. Stop the built-in SDR capture and
    // any readsb left over from the ADSB page before acarsdec opens it.
    if (sdr_.isCapturing()) sdr_.stopCapture();
    sdr_.close();
    std::system("pkill -9 readsb 2>/dev/null");
    usleep(400000);

    loadConfig();
    view_ = View::List;
    selected_index_ = 0;
    detail_seq_ = 0;
    detail_scroll_ = 0;
    help_visible_ = false;
    last_render_ = std::chrono::steady_clock::now();
    startDecoder();
    render();
}

void AcarsPage::onExit() {
    std::cout << "AcarsPage: Exiting ACARS mode" << std::endl;
    controller_.stop();
}

void AcarsPage::update(float delta_time) {
    updateHoldRepeat(delta_time);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_render_ < std::chrono::milliseconds(100)) return;
    last_render_ = now;
    render();
}

void AcarsPage::applyNavKey(AppKeyCode key) {
    if (view_ == View::List) {
        const int count = static_cast<int>(controller_.messageCount());
        if (key == APPKEY_UP) {
            if (selected_index_ > 0) --selected_index_;
        } else if (key == APPKEY_DOWN) {
            if (selected_index_ + 1 < count) ++selected_index_;
        }
        return;
    }

    if (key == APPKEY_UP) {
        if (detail_scroll_ > 0) --detail_scroll_;
    } else if (key == APPKEY_DOWN) {
        ++detail_scroll_;
    } else if (key == APPKEY_LEFT || key == APPKEY_RIGHT) {
        const auto messages = controller_.snapshot();
        stepDetail(messages, key == APPKEY_LEFT ? -1 : 1);
    }
}

void AcarsPage::updateHoldRepeat(float delta_time) {
    if (held_nav_key_ == APPKEY_NONE) return;
    const int hold_ms = input_.getHoldDuration(held_nav_key_);
    if (hold_ms <= 0) {
        held_nav_key_ = APPKEY_NONE;
        nav_repeat_accum_ = 0.0f;
        return;
    }
    if (hold_ms < 450) return;

    nav_repeat_accum_ += delta_time;
    const float interval = hold_ms >= 1500 ? 0.045f
                          : (hold_ms >= 900 ? 0.080f : 0.130f);
    int guard = 0;
    while (nav_repeat_accum_ >= interval && guard++ < 8) {
        nav_repeat_accum_ -= interval;
        applyNavKey(held_nav_key_);
    }
}

bool AcarsPage::handleInput(AppKeyCode key) {
    if (key != APPKEY_UP && key != APPKEY_DOWN && key != APPKEY_LEFT &&
        key != APPKEY_RIGHT) {
        held_nav_key_ = APPKEY_NONE;
        nav_repeat_accum_ = 0.0f;
    }

    if (key == APPKEY_H) {
        help_visible_ = !help_visible_;
        return true;
    }
    if (help_visible_) {
        if (key == APPKEY_ESC) help_visible_ = false;
        return true;
    }

    const auto messages = controller_.snapshot();
    const int count = static_cast<int>(messages.size());

    switch (key) {
        case APPKEY_ENTER:
            if (view_ == View::List && count > 0) {
                selected_index_ = std::max(
                    0, std::min(selected_index_, count - 1));
                detail_seq_ =
                    messages[static_cast<size_t>(selected_index_)].seq;
                detail_scroll_ = 0;
                view_ = View::Detail;
            } else if (view_ == View::Detail) {
                view_ = View::List;
            }
            return true;

        case APPKEY_LEFT:
        case APPKEY_RIGHT:
        case APPKEY_UP:
        case APPKEY_DOWN:
            if ((key == APPKEY_LEFT || key == APPKEY_RIGHT) &&
                view_ != View::Detail) {
                return false;
            }
            applyNavKey(key);
            held_nav_key_ = key;
            nav_repeat_accum_ = 0.0f;
            return true;

        case APPKEY_PAGEUP:
            if (view_ == View::Detail) {
                stepDetail(messages, -5);
            } else {
                selected_index_ = std::max(0, selected_index_ - 5);
            }
            return true;

        case APPKEY_PAGEDOWN:
            if (view_ == View::Detail) {
                stepDetail(messages, 5);
            } else {
                selected_index_ = std::min(std::max(0, count - 1),
                                           selected_index_ + 5);
            }
            return true;

        case APPKEY_R:
            restartDecoder();
            return true;

        case APPKEY_ESC:
            if (view_ == View::Detail) {
                view_ = View::List;
                return true;
            }
            return false;

        default:
            return false;
    }
}

void AcarsPage::render() {
    if (view_ == View::Detail) {
        const auto messages = controller_.snapshot();
        if (!messages.empty()) {
            int index = indexOfSeq(messages, detail_seq_);
            if (index < 0) {
                index = std::max(
                    0, std::min(selected_index_,
                                static_cast<int>(messages.size()) - 1));
                detail_seq_ = messages[static_cast<size_t>(index)].seq;
            }
            selected_index_ = index;
            ui_.renderDetail(messages[static_cast<size_t>(index)],
                             detail_scroll_);
            if (help_visible_) ui_.renderHelp();
            fb_.update();
            return;
        }
        view_ = View::List;
    }

    const auto messages = controller_.snapshot();
    selected_index_ = messages.empty()
                          ? 0
                          : std::min(selected_index_,
                                     static_cast<int>(messages.size()) - 1);
    ui_.renderList(messages, selected_index_, controller_.totalMessages(),
                   controller_.messagesPerSecond(), channels_, gain_db_,
                   controller_.isRunning(), controller_.lastError());
    if (help_visible_) ui_.renderHelp();
    fb_.update();
}
