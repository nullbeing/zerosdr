#ifndef SDR_UI_RENDERER_H
#define SDR_UI_RENDERER_H

#include "ui/base_ui_renderer.h"
#include <vector>
#include <cstdint>

// Layout constants (320x170 display)
static const int SPECTRUM_Y  = 0;
static const int SPECTRUM_H  = 65;
static const int FREQAXIS_Y  = 65;
static const int FREQAXIS_H  = 12;
static const int WATERFALL_Y = 77;
static const int WATERFALL_H = 79;
static const int STATUSBAR_Y = 156;
static const int STATUSBAR_H = 14;

/**
 * SDRUIRenderer - SDR-specific UI rendering
 *
 * Extends BaseUIRenderer with SDR spectrum analyzer specific rendering:
 * - Spectrum graph
 * - Waterfall display
 * - Frequency axis
 * - Status bar with SDR parameters
 */
struct SdrSettingsView {
    int deemphasis_us;   // 50 or 75
    bool audio_agc;
    int fft_size;
    int sample_rate;
    int selected;        // highlighted row
};

class SDRUIRenderer : public BaseUIRenderer {
public:
    explicit SDRUIRenderer(Framebuffer& fb);
    ~SDRUIRenderer();

    // SDR-specific rendering methods
    void renderSpectrum(const std::vector<float>& magnitude, uint32_t center_freq,
                       uint32_t span, uint32_t sample_rate, int demod_mode,
                       uint32_t demod_bandwidth);

    void renderWaterfall(const std::vector<float>& magnitude, uint32_t span, uint32_t sample_rate);

    void renderStatusBar(uint32_t freq, uint32_t span, int gain, const char* eq_input,
                         const char* demod_mode, float volume, bool squelch_open,
                         float squelch, bool audio_enabled, bool agc_enabled,
                         const char* step_label);

    // Colour themes for the spectrum and waterfall (M key).
    void cycleTheme();
    void setTheme(int index);
    int getTheme() const { return theme_index_; }
    int themeCount() const;
    const char* themeName() const;

    // Public helper for rendering frequency axis (used in device detection)
    void drawFreqAxis(uint32_t center_freq, uint32_t span, int y, int h);

    // Draw zeroSDR logo in spectrum area
    void drawLogo(int x, int y);

    // Override renderDialog for SDR-specific dialog rendering (with overlay)
    void renderDialog(const char* title, const char* message, const char* hint) override;

    // Full-screen key reference (toggled with H)
    void renderKeyHelp();

    // Settings tab of the same overlay
    void renderSettings(const SdrSettingsView& view);

private:
    int theme_index_ = 0;  // index into the spectrum/waterfall palettes

    // Waterfall: direct pixel buffer for the waterfall region
    std::vector<uint16_t> wf_pixels_; // WATERFALL_H * 320 pixels

    // Dynamic range tracking for waterfall color mapping
    float wf_floor_;   // dB floor (smoothed)
    float wf_ceiling_; // dB ceiling (smoothed)

    // Helper methods
    void drawHelpTabs(bool settings_active);
    void drawSpectrumGraph(const std::vector<float>& mag, int x, int y, int w, int h,
                          uint32_t center_freq, uint32_t span, uint32_t demod_bandwidth);
    void drawDbScale(int x, int y, int h, float db_min, float db_max);
    uint16_t magnitudeToColor(float norm) const;
    void formatFrequency(uint32_t freq, char* buf, size_t size) const;
};

#endif // SDR_UI_RENDERER_H
