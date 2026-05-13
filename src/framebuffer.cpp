#include "framebuffer.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

Framebuffer::Framebuffer(const char* device)
    : device_path(device), fb_fd(-1), fb_ptr(nullptr),
      width(0), height(0), screensize(0), initialized(false) {
}

Framebuffer::~Framebuffer() {
    if (fb_ptr != nullptr) {
        munmap(fb_ptr, screensize);
    }
    if (fb_fd >= 0) {
        close(fb_fd);
    }
}

bool Framebuffer::init() {
    // Open framebuffer device
    fb_fd = open(device_path, O_RDWR);
    if (fb_fd < 0) {
        std::cerr << "Error opening framebuffer device: " << device_path << std::endl;
        return false;
    }

    // Get variable screen info
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        std::cerr << "Error reading variable screen info" << std::endl;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    // Get fixed screen info
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        std::cerr << "Error reading fixed screen info" << std::endl;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    width = vinfo.xres;
    height = vinfo.yres;
    screensize = vinfo.yres_virtual * finfo.line_length;

    // Map framebuffer to memory
    fb_ptr = (uint16_t*)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_ptr == MAP_FAILED) {
        std::cerr << "Error mapping framebuffer to memory" << std::endl;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    initialized = true;
    return true;
}

void Framebuffer::clear(uint16_t color) {
    if (!initialized) return;

    for (int i = 0; i < width * height; i++) {
        fb_ptr[i] = color;
    }
}

void Framebuffer::drawPixel(int x, int y, uint16_t color) {
    if (!initialized || x < 0 || x >= width || y < 0 || y >= height) return;

    fb_ptr[y * width + x] = color;
}

void Framebuffer::drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    if (!initialized) return;

    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        drawPixel(x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void Framebuffer::drawRect(int x, int y, int w, int h, uint16_t color) {
    if (!initialized) return;

    drawLine(x, y, x + w - 1, y, color);
    drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
    drawLine(x + w - 1, y + h - 1, x, y + h - 1, color);
    drawLine(x, y + h - 1, x, y, color);
}

void Framebuffer::fillRect(int x, int y, int w, int h, uint16_t color) {
    if (!initialized) return;

    for (int j = y; j < y + h && j < height; j++) {
        for (int i = x; i < x + w && i < width; i++) {
            drawPixel(i, j, color);
        }
    }
}

void Framebuffer::drawChar(int x, int y, char c, uint16_t color) {
    if (!initialized) return;

    // Only support printable ASCII
    if (c < 0x20 || c > 0x7E) return;

    // Get font data (defined in font.h)
    extern const uint8_t font6x8[95][8];
    const uint8_t* glyph = font6x8[c - 0x20];

    // Draw 6x8 character
    for (int row = 0; row < 8; row++) {
        uint8_t line = glyph[row];
        for (int col = 0; col < 6; col++) {
            if (line & (0x80 >> col)) {
                drawPixel(x + col, y + row, color);
            }
        }
    }
}

void Framebuffer::drawText(int x, int y, const char* text, uint16_t color) {
    if (!initialized || !text) return;

    int cursor_x = x;
    while (*text) {
        drawChar(cursor_x, y, *text, color);
        cursor_x += 7; // 6 pixels + 1 pixel spacing
        text++;
    }
}

void Framebuffer::drawTextBg(int x, int y, const char* text, uint16_t fg, uint16_t bg) {
    if (!initialized || !text) return;

    // Calculate text width
    int len = 0;
    while (text[len]) len++;
    int text_width = len * 7 - 1; // Remove last spacing

    // Draw background
    fillRect(x, y, text_width, 8, bg);

    // Draw text on top
    drawText(x, y, text, fg);
}

void Framebuffer::update() {
    // For most framebuffers, no explicit update is needed
    // The memory-mapped buffer is automatically displayed
}

bool Framebuffer::saveScreenshot(const char* filename) {
    if (!initialized || fb_ptr == nullptr) {
        std::cerr << "Framebuffer not initialized" << std::endl;
        return false;
    }

    // Open output file
    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return false;
    }

    // Write PPM header (P6 format - binary RGB)
    fprintf(fp, "P6\n%d %d\n255\n", width, height);

    // Convert RGB565 to RGB888 and write
    for (int i = 0; i < width * height; i++) {
        uint16_t pixel = fb_ptr[i];

        // Extract RGB565 components
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;   // 6 bits -> 8 bits
        uint8_t b = (pixel & 0x1F) << 3;          // 5 bits -> 8 bits

        // Write RGB bytes
        fputc(r, fp);
        fputc(g, fp);
        fputc(b, fp);
    }

    fclose(fp);
    return true;
}

uint16_t Framebuffer::rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
