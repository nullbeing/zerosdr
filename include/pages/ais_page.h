#ifndef AIS_PAGE_H
#define AIS_PAGE_H

#include "ais/ais_decoder.h"
#include "ais/ais_store.h"
#include "adsb/map_background.h"
#include "core/gps_locator.h"
#include "core/ipage.h"
#include "framebuffer.h"
#include "sdr_controller.h"
#include "ui/ais_ui_renderer.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

class AisPage : public IPage {
public:
    AisPage(Framebuffer& fb, SDRController& sdr, InputHandler& input,
            AisUIRenderer& ui);
    ~AisPage() override;

    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    void onVolumeChanged(int percent) override;
    const char* getName() const override { return "AIS"; }
    const char* getDescription() const override { return "AIS Vessel Tracker"; }

private:
    enum class View { List, Plot, Console };

    void startDecodeThread();
    void stopDecodeThread();
    void loadStation();
    void selectTarget(int direction);
    void render();
    void updateRate();

    Framebuffer& fb_;
    SDRController& sdr_;
    InputHandler& input_;
    AisUIRenderer& ui_;

    ais::AisDecoder decoder_;
    ais::AisStore store_;
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
    float plot_range_nm_ = 8.0f;

    int gain_tenths_ = 496;  // 49.6 dB, the V4's maximum; AIS is weak here
    bool agc_ = false;

    uint64_t last_rate_messages_ = 0;
    time_t last_rate_time_ = 0;
    float messages_per_second_ = 0.0f;

    bool sdr_ready_ = false;
};

#endif  // AIS_PAGE_H
