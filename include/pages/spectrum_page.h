#ifndef SPECTRUM_PAGE_H
#define SPECTRUM_PAGE_H

#include "core/ipage.h"
#include "fft_processor.h"
#include "framebuffer.h"
#include "input_handler.h"
#include "sdr_controller.h"
#include "ui/base_ui_renderer.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Wideband spectrum analyzer page. Sweeps the RTL-SDR across a user supplied
// frequency range, stitches the slices into one wide spectrum and renders a
// qspectrumanalyzer-style spectrum + waterfall. Left/Right pan the view,
// Up/Down zoom, R edits the range.
class SpectrumPage : public IPage {
public:
    SpectrumPage(Framebuffer& fb, SDRController& sdr, InputHandler& input);
    ~SpectrumPage() override;

    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    const char* getName() const override { return "Spectrum"; }
    const char* getDescription() const override {
        return "Wideband swept spectrum analyzer";
    }

private:
    static constexpr int kWidth = 320;
    // Spectrum section (graph + axis) is 3/5 of the 156 px content area,
    // the waterfall is the remaining 2/5.
    static constexpr int kSpectrumH = 92;
    static constexpr int kAxisY = 92;
    static constexpr int kAxisH = 12;
    static constexpr int kWaterfallY = 104;
    static constexpr int kWaterfallH = 52;
    static constexpr int kStatusY = 156;
    static constexpr size_t kFftSize = 512;
    static constexpr uint32_t kSampleRate = 1024000;
    // The outer FFT bins roll off, so only the middle ~78% is used per slice.
    static constexpr double kUsableFraction = 0.78;

    void loadRange();
    void saveRange() const;
    void rebuildGeometry();

    void startSweep();
    void stopSweep();
    void sweepLoop();
    bool waitSamples(std::vector<uint8_t>& iq, size_t bytes);

    void openEditor();
    void applyEditor();
    void handleEditorKey(AppKeyCode key);
    void drawEditor();
    void drawDisplay();
    void drawHelpOverlay();
    void drawRoundedPanel(int x, int y, int w, int h, int radius,
                          uint16_t color, float alpha);
    void drawSpectrum();
    void drawAxis();
    void drawWaterfall();
    void drawStatusBar();
    void clearWaterfall();
    uint16_t heatColor(float t) const;
    std::string formatMhz(uint32_t hz, int decimals) const;

    Framebuffer& fb_;
    SDRController& sdr_;
    InputHandler& input_;
    BaseUIRenderer ui_;
    FFTProcessor fft_;

    uint32_t start_hz_ = 88000000;
    uint32_t stop_hz_ = 108000000;
    bool range_configured_ = false;

    size_t step_count_ = 1;
    double slice_hz_ = 0.0;
    double fft_bin_hz_ = 0.0;
    size_t slice_bins_ = 1;
    size_t total_bins_ = 1;

    std::vector<float> bins_db_;
    std::vector<float> disp_db_;
    std::vector<float> max_hold_db_;
    std::vector<float> hold_row_db_;
    std::atomic<bool> max_hold_active_{false};
    mutable std::mutex data_mutex_;
    std::vector<uint16_t> wf_pixels_;
    std::vector<float> row_db_;
    float floor_db_ = -100.0f;
    float ceil_db_ = -20.0f;
    float render_accum_ = 0.0f;

    size_t view_start_ = 0;
    size_t view_bins_ = 1;

    std::thread sweep_thread_;
    std::atomic<bool> sweep_run_{false};
    std::atomic<int> sweep_progress_{0};
    std::atomic<uint32_t> sweep_freq_{0};

    bool editing_ = false;
    bool help_visible_ = false;
    bool edit_stop_field_ = false;
    bool edit_fresh_ = true;
    std::string edit_start_;
    std::string edit_stop_;
    std::string edit_error_;
    std::string status_error_;
};

#endif  // SPECTRUM_PAGE_H
