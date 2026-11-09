#include "framebuffer.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

    // Allocate software back buffer for double-buffered drawing
    back_buffer_.resize(width * height, 0x0000);

    initialized = true;
    return true;
}

void Framebuffer::clear(uint16_t color) {
    if (!initialized) return;

    std::fill(back_buffer_.begin(), back_buffer_.end(), color);
}

void Framebuffer::drawPixel(int x, int y, uint16_t color) {
    if (!initialized || x < 0 || x >= width || y < 0 || y >= height) return;

    back_buffer_[y * width + x] = color;
}

void Framebuffer::blendPixel(int x, int y, uint16_t color, float alpha) {
    if (!initialized || x < 0 || x >= width || y < 0 || y >= height) return;
    if (alpha <= 0.0f) return;
    if (alpha >= 1.0f) {
        back_buffer_[y * width + x] = color;
        return;
    }

    const uint16_t dst = back_buffer_[y * width + x];
    const int dr = (dst >> 11) & 0x1F;
    const int dg = (dst >> 5) & 0x3F;
    const int db = dst & 0x1F;
    const int sr = (color >> 11) & 0x1F;
    const int sg = (color >> 5) & 0x3F;
    const int sb = color & 0x1F;

    const int r = dr + (int)((sr - dr) * alpha + 0.5f);
    const int g = dg + (int)((sg - dg) * alpha + 0.5f);
    const int b = db + (int)((sb - db) * alpha + 0.5f);

    back_buffer_[y * width + x] =
        (uint16_t)((r << 11) | (g << 5) | b);
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

void Framebuffer::update() {
    // Copy back buffer to the framebuffer in one shot
    // This eliminates tearing because the user sees a complete frame
    if (!initialized) return;
    memcpy(fb_ptr, back_buffer_.data(), back_buffer_.size() * sizeof(uint16_t));
}

bool Framebuffer::saveScreenshot(const char* filename) {
    if (!initialized || fb_ptr == nullptr) {
        std::cerr << "Framebuffer not initialized" << std::endl;
        return false;
    }

    // Allocate RGB888 buffer
    uint8_t* rgb_data = new uint8_t[width * height * 3];

    // Convert RGB565 to RGB888 (read from back buffer to capture last rendered frame)
    for (int i = 0; i < width * height; i++) {
        uint16_t pixel = back_buffer_[i];

        // Extract RGB565 components and expand to 8 bits
        rgb_data[i * 3 + 0] = ((pixel >> 11) & 0x1F) << 3;  // R: 5 bits -> 8 bits
        rgb_data[i * 3 + 1] = ((pixel >> 5) & 0x3F) << 2;   // G: 6 bits -> 8 bits
        rgb_data[i * 3 + 2] = (pixel & 0x1F) << 3;          // B: 5 bits -> 8 bits
    }

    // Write PNG file
    int result = stbi_write_png(filename, width, height, 3, rgb_data, width * 3);

    delete[] rgb_data;

    if (!result) {
        std::cerr << "Failed to write PNG file: " << filename << std::endl;
        return false;
    }

    return true;
}

uint16_t Framebuffer::rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
