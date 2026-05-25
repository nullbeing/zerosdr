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

void UIRenderer::renderSpectrum(const std::vector<float>& magnitude, uint32_t center_freq, uint32_t span, uint32_t sample_rate, int demod_mode, uint32_t demod_bandwidth) {
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
    drawSpectrumGraph(zoomed_mag, 0, 0, 320, SPECTRUM_H, center_freq, span, demod_bandwidth);

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

void UIRenderer::renderStatusBar(uint32_t freq, uint32_t span, int gain, const char* freq_input,
                                  const char* demod_mode, float volume, bool squelch_open, bool audio_enabled,
                                  bool agc_enabled) {
    // Background
    fb.fillRect(0, STATUSBAR_Y, 320, STATUSBAR_H, COLOR_STATUSBAR);

    // If frequency input mode is active, show input buffer
    if (freq_input != nullptr && freq_input[0] != '\0') {
        char input_display[32];
        snprintf(input_display, sizeof(input_display), "Freq: %s MHz", freq_input);
        fb.drawText(2, STATUSBAR_Y + 3, input_display, COLOR_TEXT_BRIGHT);
        return;  // Skip normal status display
    }

    // Layout: Frequency - DemodMode - Span(center) - Volume - Gain(right)
    // Example: 99.3M  FM  3.2M  ▰▰▰▰▱▱▱▱  +20dB

    // 1. Frequency (left, bright)
    char freq_str[16];
    formatFrequency(freq, freq_str, sizeof(freq_str));
    fb.drawText(2, STATUSBAR_Y + 3, freq_str, COLOR_TEXT_BRIGHT);

    // 2. Demod mode (moved right to avoid overlap with frequency)
    if (audio_enabled) {
        fb.drawText(65, STATUSBAR_Y + 3, demod_mode, COLOR_TEXT_BRIGHT);
    } else {
        fb.drawText(65, STATUSBAR_Y + 3, "--", COLOR_TEXT_DIM);
    }

    // 3. AGC/Gain (moved right after demod mode)
    char gain_str[16];
    if (agc_enabled) {
        snprintf(gain_str, sizeof(gain_str), "AGC");
        fb.drawTextBg(95, STATUSBAR_Y + 3, gain_str, COLOR_BLACK, COLOR_WHITE);
    } else if (gain >= 0) {
        snprintf(gain_str, sizeof(gain_str), "+%ddB", gain / 10);
        fb.drawTextBg(95, STATUSBAR_Y + 3, gain_str, COLOR_BLACK, COLOR_WHITE);
    } else {
        snprintf(gain_str, sizeof(gain_str), "AUTO");
        fb.drawText(95, STATUSBAR_Y + 3, gain_str, COLOR_TEXT_DIM);
    }

    // 4. Span (center, dim)
    char span_str[16];
    if (span >= 1000000) {
        snprintf(span_str, sizeof(span_str), "%.1fM", span / 1e6);
    } else {
        snprintf(span_str, sizeof(span_str), "%dk", span / 1000);
    }
    int span_len = strlen(span_str);
    int span_width = span_len * 7;  // Approximate character width
    int span_x = (320 - span_width) / 2;  // Center horizontally
    fb.drawText(span_x, STATUSBAR_Y + 3, span_str, COLOR_TEXT_DIM);

    // 5. Volume bar (right-aligned) - with "VOL" label
    if (audio_enabled) {
        // Draw "VOL" text label
        int label_x = 320 - 40 - 2 - 24;  // 24px left of volume bar (3 chars * 7px + spacing)
        int label_y = STATUSBAR_Y + 3;
        uint16_t label_color = squelch_open ? COLOR_TEXT_BRIGHT : COLOR_TEXT_DIM;

        fb.drawText(label_x, label_y, "VOL", label_color);

        int bar_width = 40;   // Bar width
        int bar_height = 6;   // Bar height
        int bar_x = 320 - bar_width - 2;  // Right-aligned with 2px margin
        int bar_y = STATUSBAR_Y + 4;  // Slightly lower for better centering

        // Calculate filled width based on volume (0.0 - 1.0)
        int filled_width = (int)(volume * bar_width);

        // Choose color based on squelch state
        uint16_t fill_color = squelch_open ? COLOR_TEXT_BRIGHT : COLOR_TEXT_DIM;

        // Only draw filled part (no background)
        if (filled_width > 2) {  // Need at least 3 pixels for rounded effect
            // Draw main filled rectangle
            fb.fillRect(bar_x + 1, bar_y, filled_width - 2, bar_height, fill_color);

            // Draw rounded left(semi-circle effect)
            fb.fillRect(bar_x, bar_y + 1, 1, bar_height - 2, fill_color);  // Left edge
            fb.drawPixel(bar_x, bar_y + 2, fill_color);  // Rounder
            if (bar_height > 4) fb.drawPixel(bar_x, bar_y + bar_height - 3, fill_color);

            // Draw rounded right end (semi-circle effect)
            if (filled_width > 1) {
                fb.fillRect(bar_x + filled_width - 1, bar_y + 1, 1, bar_height - 2, fill_color);  // Right edge
                fb.drawPixel(bar_x + filled_width - 1, bar_y + 2, fill_color);  // Rounder
                if (bar_height > 4) fb.drawPixel(bar_x + filled_width - 1, bar_y + bar_height - 3, fill_color);
            }
        } else if (filled_width > 0) {
            // Very small volume, just draw a small indicator
            fb.fillRect(bar_x, bar_y + 2, filled_width, 2, fill_color);
        }
    }
}

void UIRenderer::drawSpectrumGraph(const std::vector<float>& mag, int x, int y, int w, int h, uint32_t center_freq, uint32_t span, uint32_t demod_bandwidth) {
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

    // Calculate demodulation bandwidth indicator
    // Use the passed demod_bandwidth parameter directly
    int center_x = x + w / 2;
    float pixels_per_hz = (float)w / span;
    int bw_half_width = (int)(demod_bandwidth / 2 * pixels_per_hz);

    int bw_left = center_x - bw_half_width;
    int bw_right = center_x + bw_half_width;

    // Draw bandwidth indicator as semi-transparent shaded region
    uint16_t shade_color = 0x2945;  // Dark blue-gray for shading
    for (int bx = bw_left; bx <= bw_right; bx++) {
        if (bx >= x && bx < x + w) {
            // Draw vertical shading lines (every other pixel for transparency effect)
            for (int sy = y; sy < y + h; sy += 2) {
                fb.drawPixel(bx, sy, shade_color);
            }
        }
    }

    // Draw center frequency indicator (red vertical line)
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

void UIRenderer::renderDialog(const char* title, const char* message, const char* hint) {
    // Dialog dimensions (positioned in waterfall area)
    int dialog_w = 280;
    int dialog_h = 70;  // Reduced height to fit in waterfall area
    int dialog_x = (320 - dialog_w) / 2;  // 20
    int dialog_y = WATERFALL_Y + (WATERFALL_H - dialog_h) / 2;  // Center in waterfall area

    // Draw semi-transparent overlay ONLY in waterfall area
    for (int y = WATERFALL_Y; y < STATUSBAR_Y; y++) {
        for (int x = 0; x < 320; x++) {
            // Darken the waterfall area only
            if (x < dialog_x || x >= dialog_x + dialog_w ||
                y < dialog_y || y >= dialog_y + dialog_h) {
                // Outside dialog: draw dark overlay
                fb.drawPixel(x, y, 0x2104);  // Very dark gray
            }
        }
    }

    // Draw dialog box background
    fb.fillRect(dialog_x, dialog_y, dialog_w, dialog_h, 0x2945);  // Dark blue-gray

    // Draw dialog border
    fb.drawRect(dialog_x, dialog_y, dialog_w, dialog_h, COLOR_TEXT_BRIGHT);
    fb.drawRect(dialog_x + 1, dialog_y + 1, dialog_w - 2, dialog_h - 2, COLOR_TEXT_BRIGHT);

    // Draw title (centered, top of dialog)
    int title_len = strlen(title);
    int title_w = title_len * 7;  // Approximate character width
    int title_x = dialog_x + (dialog_w - title_w) / 2;
    fb.drawTextBg(title_x, dialog_y + 8, title, COLOR_YELLOW, 0x2945);

    // Draw message (centered, middle of dialog)
    int msg_len = strlen(message);
    int msg_w = msg_len * 7;
    int msg_x = dialog_x + (dialog_w - msg_w) / 2;
    fb.drawText(msg_x, dialog_y + 28, message, COLOR_TEXT_BRIGHT);

    // Draw hint (centered, bottom of dialog)
    int hint_len = strlen(hint);
    int hint_w = hint_len * 7;
    int hint_x = dialog_x + (dialog_w - hint_w) / 2;
    fb.drawText(hint_x, dialog_y + 50, hint, COLOR_TEXT_DIM);
}

void UIRenderer::drawLogo(int x, int y) {
    // "zeroSDR" logo with elegant large font (9x13 pixel characters, 3px spacing)
    // Scaled up for better visibility
    // Color: subtle cyan-blue gradient

    const uint16_t logo_color_bright = 0x4FDF;  // Bright cyan #4FC3FF
    const uint16_t logo_color_mid    = 0x2D7F;  // Mid cyan #2DA7FF
    const uint16_t logo_color_dim    = 0x1C9F;  // Dim cyan #1C93FF

    // Character width: 9 pixels, height: 13 pixels, spacing: 3 pixels between chars
    // "zeroSDR" = 7 characters
    const int char_w = 9;
    const int char_h = 13;
    const int char_spacing = 3;
    const int total_width = 7 * char_w + 6 * char_spacing;  // 81 pixels

    // Center the logo horizontally
    int start_x = x + (320 - total_width) / 2;
    int start_y = y + 20;  // 20px from top of spectrum area for better centering

    // Define 9x13 bitmap font for "zeroSDR" (larger, more elegant)
    // Each character is represented as 13 rows, each row has 9 bits
    const uint16_t font_z[] = {
        0b111111111,
        0b111111111,
        0b000000011,
        0b000000110,
        0b000001100,
        0b000011000,
        0b000110000,
        0b001100000,
        0b011000000,
        0b110000000,
        0b110000000,
        0b111111111,
        0b111111111
    };

    const uint16_t font_e[] = {
        0b111111111,
        0b111111111,
        0b110000000,
        0b110000000,
        0b110000000,
        0b111111110,
        0b111111110,
        0b110000000,
        0b110000000,
        0b110000000,
        0b110000000,
        0b111111111,
        0b111111111
    };

    const uint16_t font_r[] = {
        0b111111110,
        0b111111111,
        0b110000011,
        0b110000011,
        0b110000011,
        0b111111110,
        0b111111100,
        0b110011000,
        0b110001100,
        0b110000110,
        0b110000011,
        0b110000011,
        0b110000011
    };

    const uint16_t font_o[] = {
        0b001111100,
        0b011111110,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b011111110,
        0b001111100
    };

    const uint16_t font_S[] = {
        0b011111111,
        0b111111111,
        0b110000000,
        0b110000000,
        0b110000000,
        0b011111110,
        0b001111111,
        0b000000011,
        0b000000011,
        0b000000011,
        0b000000011,
        0b111111110,
        0b111111100
    };

    const uint16_t font_D[] = {
        0b111111100,
        0b111111110,
        0b110000111,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000011,
        0b110000111,
        0b111111110,
        0b111111100
    };

    const uint16_t font_R[] = {
        0b111111110,
        0b111111111,
        0b110000011,
        0b110000011,
        0b110000011,
        0b111111110,
        0b111111100,
        0b110011000,
        0b110001100,
        0b110000110,
        0b110000011,
        0b110000011,
        0b110000011
    };

    const uint16_t* chars[] = {font_z, font_e, font_r, font_o, font_S, font_D, font_R};

    // Draw each character
    for (int ch = 0; ch < 7; ch++) {
        int cx = start_x + ch * (char_w + char_spacing);
        const uint16_t* bitmap = chars[ch];

        for (int row = 0; row < char_h; row++) {
            uint16_t line = bitmap[row];
            for (int col = 0; col < char_w; col++) {
                if (line & (1 << (char_w - 1 - col))) {
                    // Gradient effect: top rows brighter, bottom rows dimmer
                    uint16_t color;
                    if (row < 4) {
                        color = logo_color_bright;
                    } else if (row < 9) {
                        color = logo_color_mid;
                    } else {
                        color = logo_color_dim;
                    }
                    fb.drawPixel(cx + col, start_y + row, color);
                }
            }
        }
    }
}

