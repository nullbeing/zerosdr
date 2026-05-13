#include "ui_renderer.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>

UIRenderer::UIRenderer(Framebuffer& fb)
    : fb(fb), display_mode(0), wf_floor(-80.0f), wf_ceiling(-20.0f) {
    // Allocate waterfall pixel buffer (80 rows × 320 pixels)
    wf_pixels.resize(WATERFALL_H * 320, COLOR_BG_DARK);
}

UIRenderer::~UIRenderer() {
}

void UIRenderer::renderSpectrum(const std::vector<float>& magnitude, uint32_t center_freq, uint32_t span, uint32_t sample_rate) {
    if (display_mode == 2) return;  // 仅瀑布模式，跳过频谱
    if (magnitude.empty()) return;

    // Calculate zoom: extract center portion of magnitude based on span/sample_rate ratio
    float zoom_ratio = (float)span / sample_rate;
    if (zoom_ratio > 1.0f) zoom_ratio = 1.0f;
    if (zoom_ratio < 0.01f) zoom_ratio = 0.01f;  // Minimum 1% zoom

    size_t total_bins = magnitude.size();
    size_t display_bins = (size_t)(total_bins * zoom_ratio);
    if (display_bins < 4) display_bins = 4;  // Minimum 4 bins

    size_t start_bin = (total_bins - display_bins) / 2;
    std::vector<float> zoomed_mag(magnitude.begin() + start_bin,
                                   magnitude.begin() + start_bin + display_bins);

    // Find min/max for dB scale
    float min_mag = *std::min_element(zoomed_mag.begin(), zoomed_mag.end());
    float max_mag = *std::max_element(zoomed_mag.begin(), zoomed_mag.end());

    // Draw dB scale on left side
    drawDbScale(2, 10, SPECTRUM_H - 12, min_mag, max_mag);

    // Draw spectrum graph with zoomed data
    drawSpectrumGraph(zoomed_mag, 0, 0, 320, SPECTRUM_H);

    // Draw frequency axis below spectrum
    drawFreqAxis(center_freq, span, FREQAXIS_Y, FREQAXIS_H);
}

void UIRenderer::renderWaterfall(const std::vector<float>& magnitude) {
    if (display_mode == 1) return;  // 仅频谱模式，跳过瀑布
    if (magnitude.empty()) return;

    int width = 320;

    // Update dynamic range (exponential smoothing)
    float frame_min = *std::min_element(magnitude.begin(), magnitude.end());
    float frame_max = *std::max_element(magnitude.begin(), magnitude.end());
    wf_floor   = 0.95f * wf_floor   + 0.05f * frame_min;
    wf_ceiling = 0.95f * wf_ceiling + 0.05f * frame_max;
    float range = wf_ceiling - wf_floor;
    if (range < 1.0f) range = 1.0f;

    // Shift existing waterfall down by 1 row (memmove for performance)
    if (WATERFALL_H > 1) {
        memmove(&wf_pixels[width], &wf_pixels[0], (WATERFALL_H - 1) * width * sizeof(uint16_t));
    }

    // Render new line at top (row 0 of wf_pixels)
    size_t mag_size = magnitude.size();
    for (int x = 0; x < width; x++) {
        size_t idx = (x * mag_size) / width;
        if (idx >= mag_size) idx = mag_size - 1;

        float norm = (magnitude[idx] - wf_floor) / range;
        norm = std::max(0.0f, std::min(1.0f, norm));

        wf_pixels[x] = magnitudeToColor(norm);
    }

    // Blit waterfall buffer to framebuffer at y=WATERFALL_Y
    for (int row = 0; row < WATERFALL_H; row++) {
        for (int x = 0; x < width; x++) {
            fb.drawPixel(x, WATERFALL_Y + row, wf_pixels[row * width + x]);
        }
    }
}

void UIRenderer::renderStatusBar(uint32_t freq, uint32_t span, int gain, const char* freq_input) {
    // Background
    fb.fillRect(0, STATUSBAR_Y, 320, STATUSBAR_H, COLOR_STATUSBAR);

    // If frequency input mode is active, show input buffer
    if (freq_input != nullptr && freq_input[0] != '\0') {
        char input_display[32];
        snprintf(input_display, sizeof(input_display), "Freq: %s MHz", freq_input);
        fb.drawText(2, STATUSBAR_Y + 3, input_display, COLOR_TEXT_BRIGHT);
        return;  // Skip normal status display
    }

    // Format strings
    char freq_str[16], span_str[16], gain_str[16];
    formatFrequency(freq, freq_str, sizeof(freq_str));

    //an
    if (span >= 1000000) {
        snprintf(span_str, sizeof(span_str), "%.1fM", span / 1e6);
    } else {
        snprintf(span_str, sizeof(span_str), "%dk", span / 1000);
    }

    if (gain >= 0) {
        snprintf(gain_str, sizeof(gain_str), "+%ddB", gain / 10);
    } else {
        snprintf(gain_str, sizeof(gain_str), "AUTO");
    }

    // Draw text: freq in bright, span and gain in dim
    fb.drawText(2,   STATUSBAR_Y + 3, freq_str,  COLOR_TEXT_BRIGHT);
    fb.drawText(130, STATUSBAR_Y + 3, span_str,  COLOR_TEXT_DIM);
    fb.drawText(260, STATUSBAR_Y + 3, gain_str,  COLOR_TEXT_DIM);
}

void UIRenderer::drawSpectrumGraph(const std::vector<float>& mag, int x, int y, int w, int h) {
    if (mag.empty()) return;

    float min_mag = *std::min_element(mag.begin(), mag.end());
    float max_mag = *std::max_element(mag.begin(), mag.end());
    float range = max_mag - min_mag;
    if (range < 1.0f) range = 1.0f;

    // Draw dashed grid lines
    for (int i = 1; i <= 3; i++) {
        int grid_y = y + (i * h) / 4;
        for (int gx = x; gx < x + w; gx += 4) {
            fb.drawPixel(gx,     grid_y, COLOR_GRID);
            fb.drawPixel(gx + 1, grid_y, COLOR_GRID);
        }
    }

    // Draw spectrum with gradient fill (green shading like SDR++)
    // Define green gradient colors (RGB565)
    const uint16_t COLOR_GREEN_DARK  = 0x0320;  // 深绿 #006600
    const uint16_t COLOR_GREEN_MID   = 0x07E0;  // 中绿 #00FF00
    const uint16_t COLOR_GREEN_LIGHT = 0x9FE7;  // 亮绿 #99FF99

    for (size_t i = 0; i < mag.size(); i++) {
        int px = x + (i * w) / mag.size();
        float norm = (mag[i] - min_mag) / range;
        int py = y + h - (int)(norm * (h - 2));
        py = std::max(y, std::min(y + h - 1, py));

        // Draw vertical line from bottom to signal level with gradient
        for (int fy = y + h - 1; fy >= py; fy--) {
            // Calculate gradient: bottom=dark, top=bright
            float fill_ratio = (float)(y + h - 1 - fy) / (float)(y + h - 1 - py + 1);
            uint16_t color;

            if (fill_ratio < 0.5f) {
                // Dark to mid green
                float t = fill_ratio * 2.0f;
                color = COLOR_GREEN_DARK;  // Simplified, use dark green
            } else {
                // Mid to light green
                float t = (fill_ratio - 0.5f) * 2.0f;
                color = COLOR_GREEN_MID;
            }

            // Top edge uses bright green
            if (fy == py) {
                color = COLOR_GREEN_LIGHT;
            }

            fb.drawPixel(px, fy, color);
        }
    }

    // Draw center frequency indicator (red vertical line)
    int center_x = x + w / 2;
    for (int cy = y; cy < y + h; cy++) {
        fb.drawPixel(center_x, cy, COLOR_RED);
    }
}

void UIRenderer::drawFreqAxis(uint32_t center_freq, uint32_t span, int y, int h) {
    // Clear axis area completely first
    fb.fillRect(0, y, 320, h, COLOR_BG_DARK);

    // Background for axis area
    fb.fillRect(0, y, 320, h, 0x0841); // 极深灰蓝

    // Draw 5 tick marks: left-edge, 1/4, center, 3/4, right-edge
    // Frequencies at each tick
    uint32_t freqs[5] = {
        center_freq - span / 2,
        center_freq - span / 4,
        center_freq,
        center_freq + span / 4,
        center_freq + span / 2,
    };
    int tick_x[5] = { 0, 80, 160, 240, 319 };

    char buf[16];
    for (int i = 0; i < 5; i++) {
        int tx = tick_x[i];

        // Tick mark (short vertical line)
        fb.drawPixel(tx, y + 1, COLOR_TEXT_DIM);
        fb.drawPixel(tx, y + 2, COLOR_TEXT_DIM);

        // Center tick is red and taller
        if (i == 2) {
            fb.drawPixel(tx, y + 1, COLOR_RED);
            fb.drawPixel(tx, y + 2, COLOR_RED);
            fb.drawPixel(tx, y + 3, COLOR_RED);
        }

        // Frequency label
        formatFrequency(freqs[i], buf, sizeof(buf));
        int label_len = 0;
        while (buf[label_len]) label_len++;
        int label_w = label_len * 7;

        // Clamp label position to screen
        int lx = tx - label_w / 2;
        if (lx < 0) lx = 0;
        if (lx + label_w > 320) lx = 320 - label_w;

        uint16_t color = (i == 2) ? COLOR_TEXT_BRIGHT : COLOR_TEXT_DIM;
        fb.drawText(lx, y + 4, buf, color);
    }
}

void UIRenderer::drawDbScale(int x, int y, int h, float db_min, float db_max) {
    char buf[8];
    float range = db_max - db_min;
    if (range < 1.0f) return;

    // Draw 4 labels
    for (int i = 0; i < 4; i++) {
        float db = db_max - (i * range / 3);
        int label_y = y + (i * h) / 3;
        snprintf(buf, sizeof(buf), "%+.0f", db);
        fb.drawText(x, label_y, buf, COLOR_TEXT_DIM);
    }
}

uint16_t UIRenderer::magnitudeToColor(float norm) const {
    // Professional theme: deep purple -> blue -> cyan -> white
    // 0.00-0.25: #0D0D14 (bg) -> #1A0A3A (deep purple)
    // 0.25-0.50: #1A0A3A (deep purple) -> #1A3AFF (blue)
    // 0.50-0.75: #1A3AFF (blue) -> #4FC3F7 (cyan)
    // 0.75-1.00: #4FC3F7 (cyan) -> #FFFFFF (white)
    uint8_t r, g, b;

    if (norm < 0.25f) {
        float t = norm / 0.25f;
        r = (uint8_t)(0x0D + t * (0x1A - 0x0D));
        g = (uint8_t)(0x0D + t * (0x0A - 0x0D));
        b = (uint8_t)(0x14 + t * (0x3A - 0x14));
    } else if (norm < 0.5f) {
        float t = (norm - 0.25f) / 0.25f;
        r = (uint8_t)(0x1A + t * (0x1A - 0x1A));
        g = (uint8_t)(0x0A + t * (0x3A - 0x0A));
        b = (uint8_t)(0x3A + t * (0xFF - 0x3A));
    } else if (norm < 0.75f) {
        float t = (norm - 0.5f) / 0.25f;
        r = (uint8_t)(0x1A + t * (0x4F - 0x1A));
        g = (uint8_t)(0x3A + t * (0xC3 - 0x3A));
        b = (uint8_t)(0xFF);
    } else {
        float t = (norm - 0.75f) / 0.25f;
        r = (uint8_t)(0x4F + t * (0xFF - 0x4F));
        g = (uint8_t)(0xC3 + t * (0xFF - 0xC3));
        b = 0xFF;
    }

    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void UIRenderer::formatFrequency(uint32_t freq, char* buf, size_t size) const {
    if (freq >= 1000000000) {
        snprintf(buf, size, "%.3fG", freq / 1e9);
    } else if (freq >= 1000000) {
        snprintf(buf, size, "%.3fM", freq / 1e6);
    } else if (freq >= 1000) {
        snprintf(buf, size, "%.1fk", freq / 1e3);
    } else {
        snprintf(buf, size, "%uHz", freq);
    }
}
