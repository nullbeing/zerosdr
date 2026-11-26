#ifndef APRS_PAGE_H
#define APRS_PAGE_H

#include "adsb/map_background.h"
#include "aprs/aprs_decoder.h"
#include "aprs/aprs_store.h"
#include "core/gps_locator.h"
#include "core/ipage.h"
#include "framebuffer.h"
#include "sdr_controller.h"
#include "ui/aprs_ui_renderer.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <thread>
#include <utility>
#include <vector>

class AprsPage : public IPage {
public:
    AprsPage(Framebuffer& fb, SDRController& sdr, InputHandler& input,
             AprsUIRenderer& ui);
    ~AprsPage() override;

    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    void onVolumeChanged(int percent) override;
    const char* getName() const override { return "APRS"; }
    const char* getDescription() const override {
        return "APRS Station Tracker";
    }

private:
    enum class View { List, Map, Detail, Console };

    void loadConfig();
    void saveConfig();
    void loadStation();
    void startDecodeThread();
    void stopDecodeThread();
    void updateRate();
    void stepVertical(bool up);
    void selectStation(int direction, bool positioned_only);
    void render();

    Framebuffer& fb_;
    SDRController& sdr_;
    InputHandler& input_;
    AprsUIRenderer& ui_;

    aprs::Decoder decoder_;
    aprs::Store store_;
    GpsLocator gps_;
    MapBackground map_bg_;

    std::thread decode_thread_;
    std::atomic<bool> running_{false};
    std::vector<uint8_t> sample_buffer_;

    View view_ = View::List;
    int selected_index_ = 0;
    int scroll_offset_ = 0;
    int console_scroll_ = 0;
    bool help_visible_ = false;
    float map_range_nm_ = 25.0f;

    uint32_t frequency_ = 144640000;  // China APRS
    int gain_tenths_ = 400;
    bool agc_ = true;

    // Rolling one-minute packet rate.
    std::deque<std::pair<time_t, uint64_t>> rate_history_;
    float packets_per_minute_ = 0.0f;

    bool sdr_ready_ = false;
    AppKeyCode held_vertical_key_ = APPKEY_NONE;
    AppKeyCode held_horizontal_key_ = APPKEY_NONE;
    float vertical_repeat_accum_ = 0.0f;
    float horizontal_repeat_accum_ = 0.0f;
};

#endif  // APRS_PAGE_H
