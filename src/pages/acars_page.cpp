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
    (void)delta_time;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_render_ < std::chrono::milliseconds(100)) return;
    last_render_ = now;
    render();
}

bool AcarsPage::handleInput(AppKeyCode key) {
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
                detail_scroll_ = 0;
                view_ = View::Detail;
            } else if (view_ == View::Detail) {
                view_ = View::List;
            }
            return true;

        case APPKEY_UP:
            if (view_ == View::List) {
                if (selected_index_ > 0) --selected_index_;
            } else if (detail_scroll_ > 0) {
                --detail_scroll_;
            }
            return true;

        case APPKEY_DOWN:
            if (view_ == View::List) {
                if (selected_index_ + 1 < count) ++selected_index_;
            } else {
                ++detail_scroll_;
            }
            return true;

        case APPKEY_PAGEUP:
            selected_index_ = std::max(0, selected_index_ - 5);
            return true;

        case APPKEY_PAGEDOWN:
            selected_index_ = std::min(std::max(0, count - 1),
                                       selected_index_ + 5);
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
            const int index = std::max(
                0, std::min(selected_index_,
                            static_cast<int>(messages.size()) - 1));
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
