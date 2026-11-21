#ifndef RFI_PAGE_H
#define RFI_PAGE_H
#include "core/ipage.h"
#include "fft_processor.h"
#include "sdr_controller.h"
#include "ui/base_ui_renderer.h"
#include "pages/rfi_measurement.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <string>

class RfiPage : public IPage {
public:
    RfiPage(Framebuffer& fb, SDRController& sdr);
    ~RfiPage() override { cleanup(); }
    bool init() override { return true; }
    void cleanup() override { stop(); }
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    const char* getName() const override { return "RFI"; }
    const char* getDescription() const override { return "Compare shack noise against a baseline"; }
private:
    void start();
    void stop();
    void scan();
    bool samples(std::vector<uint8_t>& iq, size_t bytes);
    void draw();
    void clearReference();
    Framebuffer& fb_;
    SDRController& sdr_;
    BaseUIRenderer ui_;
    FFTProcessor fft_{1024};
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::vector<float> captured_;
    std::string scan_error_;
    unsigned sequence_ = 0;
    unsigned displayed_ = 0;
    int band_ = 0;
    int gain_ = 20;
    float actual_gain_ = 0;
    uint32_t saved_frequency_ = 0, saved_rate_ = 0;
    int saved_gain_ = 0;
    bool saved_agc_ = true;
    bool help_ = false;
    bool delta_view_ = false;
    std::string error_;
    std::vector<float> live_;
    RfiReference reference_;
    std::vector<uint16_t> waterfall_;
    std::chrono::steady_clock::time_point last_sweep_{};
    float draw_elapsed_ = 0;
};
#endif
