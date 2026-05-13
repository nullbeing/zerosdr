#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <cstddef>
#include <linux/fb.h>

class Framebuffer {
public:
    Framebuffer(const char* device = "/dev/fb0");
    ~Framebuffer();

    bool init();
    void clear(uint16_t color = 0x0000);
    void drawPixel(int x, int y, uint16_t color);
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color);
    void drawRect(int x, int y, int w, int h, uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawChar(int x, int y, char c, uint16_t color);
    void drawText(int x, int y, const char* text, uint16_t color);
    void drawTextBg(int x, int y, const char* text, uint16_t fg, uint16_t bg);
    void update();
    bool saveScreenshot(const char* filename);

    int getWidth() const { return width; }
    int getHeight() const { return height; }
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
#define COLOR_STATUSBAR   0x18C3  // #0F1018 状态栏背景

#endif // FRAMEBUFFER_H
