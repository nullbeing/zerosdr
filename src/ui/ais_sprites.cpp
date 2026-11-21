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
    // Dim hull with a bright deck line, bow edge, keel and boot stripe.
    for (int y = 22; y <= 30; ++y) {
        const int bow = 33 + (y - 22);
        c.hline(4, bow, y, 2);
    }
    c.hline(4, 33, 22, 1);
    c.hline(4, 41, 30, 1);
    c.hline(4, 40, 31, 2);
    for (int y = 22; y <= 30; ++y) c.set(33 + (y - 22), y, 1);
    c.vline(4, 22, 30, 1);
    for (int x = 7; x <= 29; x += 4) c.set(x, 26, 3);
    c.set(40, 29, 3);
    c.set(39, 30, 3);
    c.hline(1, 3, 31, 2);
    c.set(0, 32, 2);
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
    // Containers forward of the aft bridge.
    for (int i = 0; i < 4; ++i) {
        const int x = 18 + i * 4;
        c.rect(x, 16, x + 3, 21, 2);
        c.outline(x, 16, x + 3, 21, 1);
        c.hline(x, x + 3, 16, 3);
    }
    drawBridge(c, 7, 12, 15, 21);
    drawFunnel(c, 10, 8, 12, 12);
    drawMast(c, 5, 3, 21);
    c.vline(35, 6, 21, 1);
    c.hline(33, 37, 8, 1);
    c.set(35, 5, 3);
}

void drawTanker(PixelCanvas& c) {
    drawHull(c);
    for (int i = 0; i < 3; ++i) {
        const int x = 19 + i * 5;
        c.rect(x, 18, x + 4, 21, 2);
        c.outline(x, 18, x + 4, 21, 1);
        c.hline(x, x + 4, 18, 3);
    }
    c.hline(17, 35, 20, 1);
    drawBridge(c, 7, 12, 15, 21);
    drawFunnel(c, 10, 8, 12, 12);
    drawMast(c, 5, 3, 21);
    c.vline(36, 8, 21, 1);
    c.hline(34, 38, 10, 1);
}

void drawPassenger(PixelCanvas& c) {
    drawHull(c);
    c.rect(8, 17, 34, 21, 2);
    c.outline(8, 17, 34, 21, 1);
    c.rect(12, 12, 31, 17, 2);
    c.outline(12, 12, 31, 17, 1);
    for (int x = 10; x <= 33; x += 3) c.set(x, 19, 3);
    for (int x = 14; x <= 30; x += 3) c.set(x, 14, 3);
    drawBridge(c, 29, 9, 35, 21);
    drawFunnel(c, 19, 7, 21, 11);
    drawMast(c, 7, 3, 21);
}

void drawFishing(PixelCanvas& c) {
    drawHull(c);
    drawBridge(c, 8, 14, 15, 21);
    drawFunnel(c, 11, 10, 12, 13);
    drawMast(c, 6, 3, 21);
    c.hline(15, 30, 10, 1);
    c.vline(30, 10, 18, 1);
    c.set(30, 19, 3);
    c.hline(18, 27, 23, 2);
}

void drawSail(PixelCanvas& c) {
    drawHull(c);
    const int mast_x = 22;
    c.vline(mast_x, 3, 22, 1);
    for (int y = 5; y <= 21; ++y) {
        const int half = (y - 5) * 13 / 16;
        c.hline(mast_x, mast_x + half, y, 1);
        if ((y & 1) == 0) c.hline(mast_x + 1, mast_x + half - 1, y, 3);
    }
    for (int y = 7; y <= 21; ++y) {
        const int half = (y - 7) * 9 / 14;
        c.hline(mast_x - half, mast_x, y, 1);
        if ((y & 1) == 0) c.hline(mast_x - half + 1, mast_x - 1, y, 3);
    }
    c.hline(mast_x - 3, mast_x + 3, 3, 3);
}

void drawTug(PixelCanvas& c) {
    drawHull(c);
    drawBridge(c, 16, 9, 24, 21);
    drawFunnel(c, 18, 5, 20, 9);
    drawMast(c, 27, 2, 21);
    c.vline(10, 24, 30, 3);
    c.vline(12, 24, 30, 3);
}

void drawClassB(PixelCanvas& c) {
    // Small motorboat: low hull, pointed bow, cabin and windshield.
    for (int y = 25; y <= 30; ++y) c.hline(8, 30 + (y - 25), y, 2);
    c.hline(8, 30, 25, 1);
    c.hline(8, 35, 30, 1);
    c.hline(8, 34, 31, 2);
    for (int y = 25; y <= 30; ++y) c.set(30 + (y - 25), y, 1);
    c.vline(8, 25, 30, 1);
    c.set(34, 29, 3);
    c.rect(11, 19, 20, 25, 2);
    c.outline(11, 19, 20, 25, 1);
    c.hline(12, 19, 19, 3);
    c.set(13, 21, 3);
    c.set(16, 21, 3);
    c.set(19, 21, 3);
    c.hline(20, 22, 24, 1);
    c.vline(9, 13, 25, 1);
    c.hline(8, 10, 14, 1);
    c.set(9, 12, 3);
    c.hline(2, 5, 30, 2);
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

void drawGeneric(PixelCanvas& c) {
    drawHull(c);
    drawBridge(c, 7, 12, 15, 21);
    drawFunnel(c, 10, 8, 12, 12);
    drawMast(c, 5, 3, 21);
    c.vline(33, 6, 21, 1);
    c.hline(31, 35, 8, 1);
    c.rect(21, 18, 29, 21, 2);
    c.outline(21, 18, 29, 21, 1);
    c.hline(23, 27, 20, 3);
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
        drawGeneric(canvas);
    }

    canvas.blit(fb, x, y, w, h, bright, dim, highlight);
}

}  // namespace ais_ui
