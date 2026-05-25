#ifndef UI_RENDERER_H
#define UI_RENDERER_H

#include "framebuffer.h"
#include <vector>
#include <cstdint>

// Layout constants (320x170 display)
static const int SPECTRUM_Y  = 0;
static const int SPECTRUM_H  = 65;   // 减小频谱高度
static const int FREQAXIS_Y  = 65;   // 频率坐标轴
static const int FREQAXIS_H  = 12;   // 坐标轴高度
static const int WATERFALL_Y = 77;   // 瀑布图下移
static const int WATERFALL_H = 79;   // 调整瀑布图高度
static const int STATUSBAR_Y = 156;
static const int STATUSBAR_H = 14;

class UIRenderer {
public:
    UIRenderer(Framebuffer& fb);
    ~UIRenderer();

    // Main render calls
    void renderSpectrum(const std::vector<float>& magnitude, uint32_t center_freq, uint32_t span, uint32_t sample_rate, int demod_mode, uint32_t demod_bandwidth);
    void renderWaterfall(const std::vector<float>& magnitude);
    void renderStatusBar(uint32_t freq, uint32_t span, int gain, const char* freq_input,
                         const char* demod_mode, float volume, bool squelch_open, bool audio_enabled,
                         bool agc_enabled);

    void setMode(int mode) { display_mode = mode; }
    int  getMode() const   { return display_mode; }

    // Dialog rendering
    void renderDialog(const char* title, const char* message, const char* hint);

    // Public helper for rendering frequency axis (used in device detection)
    void drawFreqAxis(uint32_t center_freq, uint32_t span, int y, int h);

    // Draw zeroSDR logo in spectrum area
    void drawLogo(int x, int y);

private:
    Framebuffer& fb;
    int display_mode; // reserved for future use

    // Waterfall: direct pixel buffer for the waterfall region
    // We store the rendered RGB565 rows so we can memmove them
    std::vector<uint16_t> wf_pixels; // WATERFALL_H * 320 pixels

    // Dynamic range tracking for waterfall colour mapping
    float wf_floor;   // dB floor (smoothed)
    float wf_ceiling; // dB ceiling (smoothed)

    // Helpers
    void drawSpectrumGraph(const std::vector<float>& mag, int x, int y, int w, int h, uint32_t center_freq, uint32_t span, uint32_t demod_bandwidth);
    void drawDbScale(int x, int y, int h, float db_min, float db_max);
    uint16_t magnitudeToColor(float norm) const;
    void formatFrequency(uint32_t freq, char* buf, size_t size) const;
};

#endif // UI_RENDERER_H
