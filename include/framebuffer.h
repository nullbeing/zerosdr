#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <cstddef>
#include <vector>
#include <linux/fb.h>

class Framebuffer {
public:
    Framebuffer(const char* device = "/dev/fb0");
    ~Framebuffer();

    bool init();
    void clear(uint16_t color = 0x0000);
    void drawPixel(int x, int y, uint16_t color);
    // Alpha-blend a colour over the existing back-buffer pixel (0.0 .. 1.0).
    void blendPixel(int x, int y, uint16_t color, float alpha);
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color);
    void drawRect(int x, int y, int w, int h, uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void update();
    bool saveScreenshot(const char* filename);

    int getWidth() const { return width; }
    int getHeight() const { return height; }

    // Direct access to the software back buffer (used to freeze a frame while
    // the exit prompt is shown).
    uint16_t* pixels() { return back_buffer_.data(); }
    const uint16_t* pixels() const { return back_buffer_.data(); }
    size_t pixelCount() const { return back_buffer_.size(); }
    bool isInitialized() const { return initialized; }

private:
    const char* device_path;
    int fb_fd;
    uint16_t* fb_ptr;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int width;
    int height;
    size_t screensize;
    bool initialized;

    // Software back buffer for double-buffered drawing (eliminates tearing)
    std::vector<uint16_t> back_buffer_;

    uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b);
};

// Color definitions (RGB565 format)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

// Professional theme colors
#define COLOR_BG_DARK     0x1082  // #0D0D14 深灰蓝背景
#define COLOR_SPECTRUM    0x4FDF  // #4FC3F7 冷蓝频谱线
#define COLOR_GRID        0x2124  // #1A2040 暗蓝网格
#define COLOR_TEXT_DIM    0x6B4D  // #6A7090 暗文字
#define COLOR_TEXT_BRIGHT 0x9E9F  // #9DB4FF 亮文字
#define COLOR_TEXT        0x9E9F  // Default text color (same as bright)
#define COLOR_STATUSBAR   0x18C3  // #0F1018 状态栏背景

// Dialog colors
#define COLOR_DIALOG_BG     0x2945  // Dark blue-gray for dialog background
#define COLOR_DIALOG_BORDER 0x9E9F  // Bright text color for dialog border

// UI element colors
#define COLOR_SELECTED    0x2945  // Selected item background (dark blue-gray)

// ADSB page colors
#define COLOR_ADSB_STATUSBAR_BG   0x1B53  // #3365A6 蓝色状态栏背景
#define COLOR_ADSB_STATUSBAR_TEXT 0xFEE7  // #FDE84F 黄色状态栏文字

// PFD instrument palette (attitude indicator, tapes, readouts)
#define COLOR_PFD_SKY_TOP     0x1A71  // #1E4E8C deep sky, top of the ball
#define COLOR_PFD_SKY_LOW     0x5D5D  // #5FA8E8 pale sky at the horizon
#define COLOR_PFD_GROUND_TOP  0x9B45  // #9C6B2F tan ground at the horizon
#define COLOR_PFD_GROUND_LOW  0x59E2  // #5C3C17 dark ground at the bottom
#define COLOR_PFD_BEZEL       0x31E9  // #343C4C instrument bezel
#define COLOR_PFD_BEZEL_HI    0x6BD2  // #6E7A90 bezel highlight
#define COLOR_PFD_PANEL       0x0862  // #0A0E16 instrument panel face
#define COLOR_PFD_TAPE_BG     0x0021  // #05070C tape background
#define COLOR_PFD_TAPE_LINE   0x2989  // #2A3348 tape frame
#define COLOR_PFD_TAPE_TICK   0x8CB5  // #8894AC tape ticks and labels
#define COLOR_PFD_LADDER      0xDF1E  // #D8E0F0 pitch ladder
#define COLOR_PFD_SEG_ON      0xFE09  // #FFC24A lit seven-segment digits
#define COLOR_PFD_SEG_DIM     0x8B44  // #8C6A24 neighbour digits on a wheel

// Radar page colors
#define COLOR_RADAR_BG        0x0841  // #081020 Dark blue-black background
#define COLOR_RADAR_GRID      0x2945  // #294580 Dim cyan for range rings
#define COLOR_RADAR_SWEEP     0x07E0  // #00FF00 Bright green for sweep line
#define COLOR_RADAR_BLIP      0xFFFF  // #FFFFFF White for aircraft blips
#define COLOR_RADAR_SELECTED  0x001F  // #0000FF Blue for selected aircraft
#define COLOR_RADAR_TEXT      0x9E9F  // #9DB4FF Bright text for labels

#endif // FRAMEBUFFER_H
