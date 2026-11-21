#include "ui/ais_sprites.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace ais_ui {
namespace {

class PixelCanvas {
public:
    static constexpr int W = 48;
    static constexpr int H = 36;

    void set(int x, int y, uint8_t value) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        pixels_[static_cast<size_t>(y * W + x)] = value;
    }

    void hline(int x0, int x1, int y, uint8_t value) {
        if (x0 > x1) std::swap(x0, x1);
        for (int x = x0; x <= x1; ++x) set(x, y, value);
    }

    void vline(int x, int y0, int y1, uint8_t value) {
        if (y0 > y1) std::swap(y0, y1);
        for (int y = y0; y <= y1; ++y) set(x, y, value);
    }

    void rect(int x0, int y0, int x1, int y1, uint8_t value) {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        for (int y = y0; y <= y1; ++y) hline(x0, x1, y, value);
    }

    void outline(int x0, int y0, int x1, int y1, uint8_t value) {
        hline(x0, x1, y0, value);
        hline(x0, x1, y1, value);
        vline(x0, y0, y1, value);
        vline(x1, y0, y1, value);
    }

    void dither(int x0, int y0, int x1, int y1,
                uint8_t a, uint8_t b) {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                set(x, y, ((x + y) & 1) ? a : b);
            }
        }
    }

    uint8_t get(int x, int y) const {
        if (x < 0 || x >= W || y < 0 || y >= H) return 0;
        return pixels_[static_cast<size_t>(y * W + x)];
    }

    void blit(Framebuffer& fb, int x, int y, int w, int h,
              uint16_t bright, uint16_t dim, uint16_t highlight) const {
        const int scale = 2;
        const int dest_x = x + (w - W * scale) / 2;
        const int dest_y = y + (h - H * scale) / 2;
        const uint16_t palette[4] = {0, bright, dim, highlight};
        for (int py = 0; py < H; ++py) {
            for (int px = 0; px < W; ++px) {
                const uint8_t value = get(px, py);
                if (value == 0) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        fb.drawPixel(dest_x + px * scale + dx,
                                     dest_y + py * scale + dy,
                                     palette[value & 3]);
                    }
                }
            }
        }
    }

private:
    std::array<uint8_t, W * H> pixels_{};
};

void drawHull(PixelCanvas& c) {
    for (int y = 22; y <= 29; ++y) {
        const int bow = 34 + (y - 22);
        c.hline(6, bow, y, 1);
    }
    c.hline(6, 41, 22, 3);
    c.hline(3, 6, 27, 1);
    c.hline(3, 5, 26, 2);
    c.hline(6, 40, 30, 2);
    c.hline(6, 39, 31, 2);
    c.dither(8, 28, 38, 29, 2, 1);
    for (int x = 10; x <= 30; x += 4) c.set(x, 25, 3);
}

void drawBridge(PixelCanvas& c, int x0, int y0, int x1, int y1) {
    c.rect(x0, y0, x1, y1, 2);
    c.outline(x0, y0, x1, y1, 1);
    for (int x = x0 + 2; x < x1; x += 2) c.set(x, y0 + 2, 3);
    c.hline(x0 + 1, x1 - 1, y1 - 1, 1);
}

void drawFunnel(PixelCanvas& c, int x0, int y0, int x1, int y1) {
    c.rect(x0, y0, x1, y1, 2);
    c.outline(x0, y0, x1, y1, 1);
    c.hline(x0, x1, y0, 3);
}

void drawMast(PixelCanvas& c, int x, int y0, int y1) {
    c.vline(x, y0, y1, 1);
    c.hline(x - 2, x + 2, y0 + 2, 1);
    c.set(x, y0 - 1, 3);
    c.set(x - 1, y0, 3);
}

void drawCargo(PixelCanvas& c) {
    drawHull(c);
    for (int i = 0; i < 5; ++i) {
        const int x = 9 + i * 5;
        c.rect(x, 17, x + 3, 22, 2);
        c.outline(x, 17, x + 3, 22, 1);
        c.hline(x, x + 3, 17, 3);
    }
    drawBridge(c, 33, 14, 40, 22);
    drawFunnel(c, 36, 9, 38, 13);
    drawMast(c, 42, 6, 22);
    c.hline(39, 45, 8, 1);
    c.dither(10, 30, 34, 31, 2, 1);
}

void drawTanker(PixelCanvas& c) {
    drawHull(c);
    for (int i = 0; i < 4; ++i) {
        const int x = 9 + i * 6;
        c.rect(x, 18, x + 4, 22, 2);
        c.hline(x, x + 4, 18, 3);
        c.hline(x + 1, x + 3, 20, 1);
    }
    c.hline(8, 32, 17, 1);
    c.hline(8, 32, 23, 1);
    drawBridge(c, 33, 13, 40, 22);
    drawFunnel(c, 36, 8, 38, 12);
    drawMast(c, 42, 5, 22);
    c.vline(30, 17, 22, 1);
    c.dither(9, 30, 34, 31, 2, 1);
}

void drawPassenger(PixelCanvas& c) {
    drawHull(c);
    c.rect(10, 18, 38, 22, 2);
    c.outline(10, 18, 38, 22, 1);
    c.rect(14, 13, 34, 18, 2);
    c.outline(14, 13, 34, 18, 1);
    for (int x = 12; x <= 36; x += 3) {
        c.set(x, 20, 3);
        c.set(x, 15, 3);
    }
    drawBridge(c, 34, 11, 41, 22);
    drawFunnel(c, 36, 6, 38, 10);
    drawMast(c, 42, 4, 22);
    c.hline(39, 45, 7, 1);
    c.dither(11, 30, 35, 31, 2, 1);
}

void drawFishing(PixelCanvas& c) {
    drawHull(c);
    c.rect(25, 16, 33, 22, 2);
    c.outline(25, 16, 33, 22, 1);
    c.set(27, 18, 3);
    c.set(30, 18, 3);
    drawFunnel(c, 31, 12, 33, 15);
    drawMast(c, 22, 5, 22);
    c.hline(22, 30, 7, 1);
    c.hline(22, 30, 8, 1);
    c.dither(10, 28, 26, 29, 2, 1);
    c.hline(4, 8, 29, 2);
}

void drawSail(PixelCanvas& c) {
    drawHull(c);
    const int mast_x = 24;
    c.vline(mast_x, 4, 22, 1);
    for (int y = 6; y <= 20; ++y) {
        const int half = (y - 6) * 12 / 14;
        c.hline(mast_x, mast_x + half, y, 1);
        if ((y & 1) == 0) c.hline(mast_x + 1, mast_x + half - 1, y, 3);
    }
    for (int y = 8; y <= 20; ++y) {
        const int half = (y - 8) * 10 / 12;
        c.hline(mast_x - half, mast_x, y, 1);
        if ((y & 1) == 0) c.hline(mast_x - half + 1, mast_x - 1, y, 3);
    }
    c.hline(mast_x - 3, mast_x + 3, 4, 3);
    c.hline(6, 10, 29, 2);
}

void drawTug(PixelCanvas& c) {
    drawHull(c);
    c.rect(22, 11, 32, 22, 2);
    c.outline(22, 11, 32, 22, 1);
    for (int x = 24; x <= 30; x += 2) c.set(x, 14, 3);
    drawFunnel(c, 28, 6, 30, 10);
    drawMast(c, 34, 3, 22);
    c.vline(18, 24, 29, 3);
    c.vline(20, 24, 29, 3);
    c.hline(14, 21, 30, 1);
    c.dither(10, 30, 34, 31, 2, 1);
}

void drawClassB(PixelCanvas& c) {
    for (int y = 25; y <= 30; ++y) c.hline(14, 36 - (30 - y), y, 1);
    c.hline(14, 36, 25, 3);
    c.hline(14, 34, 31, 2);
    c.rect(25, 19, 32, 25, 2);
    c.outline(25, 19, 32, 25, 1);
    c.set(27, 21, 3);
    c.set(30, 21, 3);
    c.vline(20, 18, 25, 1);
    c.hline(19, 21, 18, 1);
    c.set(24, 17, 3);
    c.dither(16, 29, 32, 30, 2, 1);
    c.hline(6, 10, 30, 2);
}

void drawAton(PixelCanvas& c) {
    c.hline(4, 44, 30, 2);
    c.hline(6, 42, 31, 2);
    c.rect(22, 15, 28, 29, 2);
    for (int y = 15; y <= 29; ++y) {
        c.set(22, y, 1);
        c.set(28, y, 1);
        if (((y - 15) / 3) & 1) c.hline(23, 27, y, 3);
    }
    c.rect(24, 8, 26, 14, 2);
    c.outline(24, 8, 26, 14, 1);
    c.hline(24, 26, 8, 3);
    c.set(25, 6, 3);
    c.hline(18, 32, 29, 1);
    c.hline(4, 20, 31, 2);
}

void drawBase(PixelCanvas& c) {
    c.hline(4, 44, 30, 2);
    c.rect(18, 20, 30, 30, 2);
    c.outline(18, 20, 30, 30, 1);
    for (int x = 20; x <= 28; x += 3) c.set(x, 23, 3);
    c.rect(23, 5, 25, 20, 1);
    c.set(24, 2, 3);
    c.hline(21, 27, 8, 1);
    c.hline(19, 29, 13, 1);
    c.hline(4, 18, 31, 2);
    c.hline(30, 44, 31, 2);
}

}  // namespace

void drawDossierShip(Framebuffer& fb, int x, int y, int w, int h,
                     const ais::VesselState& vessel,
                     uint16_t bright, uint16_t dim, uint16_t highlight) {
    PixelCanvas canvas;
    const int type = vessel.ship_type;

    if (vessel.base_station) {
        drawBase(canvas);
    } else if (vessel.aton) {
        drawAton(canvas);
    } else if (vessel.class_b && (type == 36 || type == 37)) {
        drawSail(canvas);
    } else if (type == 36 || type == 37) {
        drawSail(canvas);
    } else if (type == 30) {
        drawFishing(canvas);
    } else if (type == 31 || type == 32 || type == 52) {
        drawTug(canvas);
    } else if (type >= 60 && type < 70) {
        drawPassenger(canvas);
    } else if (type >= 70 && type < 80) {
        drawCargo(canvas);
    } else if (type >= 80 && type < 90) {
        drawTanker(canvas);
    } else if (vessel.class_b) {
        drawClassB(canvas);
    } else {
        drawCargo(canvas);
    }

    canvas.blit(fb, x, y, w, h, bright, dim, highlight);
}

}  // namespace ais_ui
