#include "ui/ais_portrait.h"
#include "font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ais_ui {
namespace {

constexpr int CX = 100;
constexpr int CY = 86;
constexpr int R = 78;
constexpr int R_OUTER = 84;
constexpr int HORIZON = 92;
constexpr int WATERLINE = 109;
constexpr int DECK = 96;

constexpr int STERN = 30;
constexpr int BOW_DECK = 156;
constexpr int BOW_TOP = 90;
constexpr int KEEL = 118;

constexpr double kPi = 3.14159265358979323846;

constexpr uint16_t RGB565(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) |
                                 ((b & 0xF8) >> 3));
}

uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    const int ar = (a >> 11) & 0x1F;
    const int ag = (a >> 5) & 0x3F;
    const int ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F;
    const int bg = (b >> 5) & 0x3F;
    const int bb = b & 0x1F;
    const int r = ar + static_cast<int>((br - ar) * t + 0.5f);
    const int g = ag + static_cast<int>((bg - ag) * t + 0.5f);
    const int bl = ab + static_cast<int>((bb - ab) * t + 0.5f);
    return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

void drawText6(Framebuffer& fb, int x, int y, const char* text, uint16_t color) {
    if (!text) return;
    int cursor = x;
    while (*text) {
        const unsigned char ch = static_cast<unsigned char>(*text++);
        if (ch >= 0x20 && ch <= 0x7E) {
            const uint8_t* glyph = font6x8[ch - 0x20];
            for (int row = 0; row < FONT_6X8_HEIGHT; ++row) {
                const uint8_t line = glyph[row];
                for (int col = 0; col < FONT_6X8_WIDTH; ++col) {
                    if (line & (0x80 >> col)) {
                        fb.drawPixel(cursor + col, y + row, color);
                    }
                }
            }
        }
        cursor += FONT_6X8_WIDTH + 1;
    }
}

void circleOutline(Framebuffer& fb, int cx, int cy, int radius,
                   uint16_t color) {
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        fb.drawPixel(cx + x, cy + y, color);
        fb.drawPixel(cx + y, cy + x, color);
        fb.drawPixel(cx - y, cy + x, color);
        fb.drawPixel(cx - x, cy + y, color);
        fb.drawPixel(cx - x, cy - y, color);
        fb.drawPixel(cx - y, cy - x, color);
        fb.drawPixel(cx + y, cy - x, color);
        fb.drawPixel(cx + x, cy - y, color);
        ++y;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
}

/** Clipped drawing surface: everything is masked by the porthole. */
struct Canvas {
    Framebuffer& fb;
    uint16_t accent;
    uint16_t dim;
    uint16_t highlight;
    uint16_t amber;

    bool inside(int x, int y) const {
        const int dx = x - CX;
        const int dy = y - CY;
        return dx * dx + dy * dy <= (R - 2) * (R - 2);
    }

    void put(int x, int y, uint16_t c) const {
        if (inside(x, y)) fb.drawPixel(x, y, c);
    }

    void blend(int x, int y, uint16_t c, float alpha) const {
        if (inside(x, y)) fb.blendPixel(x, y, c, alpha);
    }

    void hline(int x0, int x1, int y, uint16_t c) const {
        if (x0 > x1) std::swap(x0, x1);
        for (int x = x0; x <= x1; ++x) put(x, y, c);
    }

    void vline(int x, int y0, int y1, uint16_t c) const {
        if (y0 > y1) std::swap(y0, y1);
        for (int y = y0; y <= y1; ++y) put(x, y, c);
    }

    void rect(int x0, int y0, int x1, int y1, uint16_t c) const {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        for (int y = y0; y <= y1; ++y) hline(x0, x1, y, c);
    }

    void outline(int x0, int y0, int x1, int y1, uint16_t c) const {
        hline(x0, x1, y0, c);
        hline(x0, x1, y1, c);
        vline(x0, y0, y1, c);
        vline(x1, y0, y1, c);
    }

    void line(int x0, int y0, int x1, int y1, uint16_t c) const {
        int dx = std::abs(x1 - x0);
        int sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0);
        int sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        while (true) {
            put(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * error;
            if (e2 >= dy) { error += dy; x0 += sx; }
            if (e2 <= dx) { error += dx; y0 += sy; }
        }
    }
};

uint16_t seaColorAt(int y) {
    const float t = static_cast<float>(y - HORIZON) /
                    static_cast<float>((CY + R) - HORIZON);
    return lerp565(0x0ACE, 0x0147, std::max(0.0f, std::min(1.0f, t)));
}

int deckY(int x) {
    return DECK - (x - STERN) * (DECK - BOW_TOP) / (BOW_DECK - STERN);
}

/** Rows of the hull: deck sheer at the bow, raked stem and a short keel. */
void hullEdges(int y, int& left, int& right) {
    if (y < DECK) {
        left = STERN + (DECK - y) * (BOW_DECK - STERN) / (DECK - BOW_TOP);
        right = BOW_DECK + (y - BOW_TOP) * 6 / (DECK - BOW_TOP);
    } else if (y <= WATERLINE) {
        left = STERN - (y - DECK) * 3 / (KEEL - DECK);
        right = BOW_DECK + 6 + (y - DECK) * 8 / (WATERLINE - DECK);
    } else {
        left = STERN - 3 - (y - WATERLINE);
        right = BOW_DECK + 14 - (y - WATERLINE) * 12 / (KEEL - WATERLINE);
    }
}

void drawHull(Canvas& c, bool whiteHull, uint16_t upper, uint16_t lower,
              uint16_t bootTop, uint16_t stripeOne, uint16_t stripeTwo) {
    for (int y = BOW_TOP; y <= KEEL; ++y) {
        int left = 0;
        int right = 0;
        hullEdges(y, left, right);
        if (right <= left) continue;
        uint16_t color;
        if (y <= WATERLINE) {
            const float t = static_cast<float>(y - BOW_TOP) /
                            static_cast<float>(WATERLINE - BOW_TOP);
            color = whiteHull ? lerp565(stripeOne, stripeTwo, t)
                              : lerp565(upper, lower, t);
        } else {
            const float t = static_cast<float>(y - WATERLINE) /
                            static_cast<float>(KEEL - WATERLINE);
            color = (y <= WATERLINE + 1)
                        ? bootTop
                        : lerp565(0x3800, 0x1800, t);
        }
        c.hline(left, right, y, color);
    }

    // Sheer strake just under the deck edge.
    for (int x = STERN + 2; x <= BOW_DECK - 2; ++x) {
        const int top = deckY(x);
        for (int k = 2; k <= 5; ++k) {
            c.put(x, top + k, whiteHull ? 0xE71C : 0x39E7);
        }
    }

    // Deck sheer and stem highlight.
    c.line(STERN, DECK, BOW_DECK, BOW_TOP, c.highlight);
    c.line(BOW_DECK, BOW_TOP, BOW_DECK + 6, DECK, c.highlight);
    c.line(BOW_DECK + 6, DECK, BOW_DECK + 8, WATERLINE, c.highlight);

    // Hull plating seams (steel hulls only).
    if (!whiteHull) {
        for (int x = STERN + 8; x <= BOW_DECK - 6; x += 12) {
            const int top = deckY(x) + 3;
            c.vline(x, top, WATERLINE - 1, 0x2965);
        }
    } else {
        // Navy sheer stripe to break up the white topsides.
        for (int x = STERN; x <= BOW_DECK + 4; ++x) {
            const int top = deckY(x);
            c.put(x, top + 4, 0x02AA);
            c.put(x, top + 5, 0x02AA);
        }
    }

    if (stripeOne != stripeTwo) {
        c.hline(STERN - 3, BOW_DECK + 8, WATERLINE - 2,
                whiteHull ? c.dim : 0x7B60);
    }

    // Submerged hull fades into the water.
    for (int y = WATERLINE; y <= KEEL; ++y) {
        int left = 0;
        int right = 0;
        hullEdges(y, left, right);
        if (right <= left) continue;
        const float depth = static_cast<float>(y - WATERLINE) /
                            static_cast<float>(KEEL - WATERLINE);
        const uint16_t sea = seaColorAt(y);
        const float alpha = 0.42f + 0.30f * depth;
        for (int x = left; x <= right; ++x) c.blend(x, y, sea, alpha);
    }

    // Waterline and a little foam.
    c.hline(STERN - 4, BOW_DECK + 8, WATERLINE, 0x3C9E);
    for (int x = STERN - 2; x < BOW_DECK + 6; x += 9) {
        c.hline(x, x + 5, WATERLINE + 1, 0x2A8A);
        c.put(x + 2, WATERLINE + 2, 0x1A6B);
    }
}

void drawBridgeWindows(Canvas& c, int x0, int x1, int y, int step) {
    for (int x = x0; x <= x1; x += step) {
        c.rect(x, y, x + 2, y + 1, c.amber);
    }
}

void drawCargo(Canvas& c) {
    drawHull(c, false, 0x3186, 0x18E3, 0x8000, 0, 0);

    // Deck cargo: two tiers of containers.
    const uint16_t box[5] = {0x8200, 0x02AA, 0x7B60, 0x2965, 0x03A6};
    for (int i = 0; i < 5; ++i) {
        const int x = 86 + i * 12;
        const int base = deckY(x);
        c.rect(x, base - 10, x + 10, base - 1, box[i % 5]);
        c.outline(x, base - 10, x + 10, base - 1, 0x18E3);
        c.hline(x + 1, x + 9, base - 10, c.highlight);
        for (int d = 1; d < 3; ++d) {
            c.vline(x + d * 3, base - 9, base - 2, 0x18E3);
        }
    }
    for (int i = 0; i < 4; ++i) {
        const int x = 92 + i * 13;
        const int base = deckY(x) - 10;
        c.rect(x, base - 8, x + 11, base - 1, box[(i + 2) % 5]);
        c.outline(x, base - 8, x + 11, base - 1, 0x18E3);
        c.hline(x + 1, x + 10, base - 8, c.highlight);
    }

    // Aft accommodation block and bridge.
    c.rect(34, 70, 74, DECK - 1, 0x39C7);
    c.rect(37, 70, 71, DECK - 1, 0xD69A);
    c.outline(34, 70, 74, DECK - 1, c.dim);
    drawBridgeWindows(c, 39, 67, 74, 8);
    drawBridgeWindows(c, 39, 67, 84, 8);
    c.rect(30, 62, 78, 70, 0x4A49);
    c.rect(32, 62, 76, 70, 0xCE79);
    c.outline(30, 62, 78, 70, c.dim);
    drawBridgeWindows(c, 33, 75, 65, 7);
    c.hline(30, 78, 62, c.highlight);

    // Funnel with a band.
    c.rect(48, 46, 60, 62, 0x2104);
    c.rect(49, 48, 59, 61, 0x18C3);
    c.hline(48, 60, 46, c.dim);
    c.hline(49, 59, 52, c.amber);
    c.hline(49, 59, 53, c.amber);
    c.hline(48, 60, 47, 0x0000);

    // Radar mast and nav lights.
    c.vline(66, 40, 62, c.dim);
    c.hline(60, 72, 46, c.dim);
    c.hline(63, 69, 42, c.highlight);
    c.put(66, 40, c.amber);
    c.put(30, 62, 0xF800);
    c.put(78, 62, 0x07E0);

    // Draft marks near the bow.
    for (int i = 0; i < 4; ++i) {
        c.hline(147, 150, 100 + i * 3, 0x9CF3);
    }

    // Foremast.
    c.vline(150, 56, deckY(150), c.dim);
    c.hline(144, 156, 64, c.dim);
    c.put(150, 56, c.amber);
}

void drawTanker(Canvas& c) {
    drawHull(c, false, 0x2966, 0x1082, 0x8000, 0, 0);

    // Aft house.
    c.rect(30, 72, 64, DECK - 1, 0x39C7);
    c.rect(33, 72, 61, DECK - 1, 0xD69A);
    c.outline(30, 72, 64, DECK - 1, c.dim);
    drawBridgeWindows(c, 35, 59, 76, 8);
    drawBridgeWindows(c, 35, 59, 86, 8);
    c.rect(28, 64, 66, 72, 0x4A49);
    c.outline(28, 64, 66, 72, c.dim);
    drawBridgeWindows(c, 31, 63, 67, 7);

    // Funnel.
    c.rect(40, 50, 50, 64, 0x2104);
    c.rect(41, 52, 49, 63, 0x03A6);
    c.hline(41, 49, 54, 0x2104);
    c.hline(41, 49, 57, c.amber);
    c.hline(40, 50, 50, 0x0000);

    c.vline(56, 44, 64, c.dim);
    c.hline(50, 62, 50, c.dim);
    c.put(56, 44, c.amber);

    // Manifold pipes running above the deck edge.
    for (int x = 64; x <= 149; ++x) {
        const int deck = deckY(x);
        c.put(x, deck - 1, 0x9CF3);
        c.put(x, deck - 2, 0x6B4D);
    }
    for (int x = 68; x <= 144; x += 16) {
        const int deck = deckY(x);
        c.vline(x, deck - 4, deck - 1, 0x9CF3);
        c.rect(x - 1, deck - 5, x + 1, deck - 4, 0x9CF3);
        c.put(x, deck - 6, c.amber);
    }

    // Catwalk, midship vent post and bow manifold.
    for (int x = 66; x <= 150; ++x) {
        c.put(x, deckY(x) + 6, 0x2965);
    }
    c.vline(118, deckY(118) - 10, deckY(118), c.dim);
    c.hline(114, 122, deckY(118) - 10, c.dim);
    const int bow_deck = deckY(150);
    c.rect(147, bow_deck - 7, 155, bow_deck, 0x9CF3);
    c.outline(147, bow_deck - 7, 155, bow_deck, c.dim);
    c.vline(151, bow_deck - 13, bow_deck - 7, c.dim);
    c.hline(148, 154, bow_deck - 13, c.dim);
    c.put(151, bow_deck - 14, c.amber);
}

void drawPassenger(Canvas& c) {
    drawHull(c, true, 0, 0, 0x8800, 0xF7BE, 0xDEDB);

    // Long superstructure, two decks.
    c.rect(36, 74, 152, DECK - 1, 0xDEFB);
    c.outline(36, 74, 152, DECK - 1, c.dim);
    c.rect(44, 62, 142, 74, 0xE71C);
    c.outline(44, 62, 142, 74, c.dim);
    c.hline(36, 152, 74, 0x9CF3);
    c.hline(44, 142, 62, c.highlight);

    for (int x = 40; x <= 148; x += 6) {
        c.rect(x, 79, x + 3, 81, 0x2965);
        c.rect(x, 88, x + 3, 90, 0x2965);
    }
    for (int x = 48; x <= 138; x += 6) {
        c.rect(x, 66, x + 3, 68, 0x2965);
    }

    // Raked bridge front.
    c.line(142, 62, 152, 74, c.dim);
    c.rect(140, 60, 150, 62, 0x39C7);
    drawBridgeWindows(c, 142, 149, 60, 4);

    // Twin funnels.
    for (int i = 0; i < 2; ++i) {
        const int x = 62 + i * 14;
        c.rect(x, 46, x + 8, 62, 0x2104);
        c.rect(x + 1, 49, x + 7, 61, 0xE71C);
        c.hline(x + 1, x + 7, 54, c.amber);
        c.hline(x + 1, x + 7, 55, c.amber);
        c.hline(x, x + 8, 46, 0x0000);
    }

    // Forward mast.
    c.vline(148, 40, 60, c.dim);
    c.hline(142, 154, 48, c.dim);
    c.put(148, 40, c.amber);

    // Deck shadow between the tiers.
    c.hline(36, 152, 75, 0x8410);
    c.hline(44, 142, 63, 0x8410);

    // A row of lifeboats under the upper deck.
    for (int x = 54; x <= 130; x += 10) {
        c.rect(x, 70, x + 6, 73, 0xFD20);
        c.outline(x, 70, x + 6, 73, 0x8200);
    }
}

void drawFishing(Canvas& c) {
    drawHull(c, true, 0, 0, 0x8000, 0xEF7D, 0xCE79);
    c.hline(STERN, BOW_DECK + 6, 101, 0x02AA);
    c.hline(STERN, BOW_DECK + 6, 102, 0x02AA);

    // Wheelhouse.
    c.rect(96, 76, 128, DECK - 1, 0xBDF7);
    c.outline(96, 76, 128, DECK - 1, c.dim);
    c.rect(99, 80, 125, 85, 0x2104);
    drawBridgeWindows(c, 100, 123, 81, 5);
    c.hline(96, 128, 76, c.highlight);

    // Mast, boom and stern gantry.
    c.vline(84, 46, deckY(84), c.dim);
    c.hline(78, 90, 54, c.dim);
    c.line(84, 58, 56, 84, c.dim);
    c.line(84, 58, 118, 70, c.dim);
    c.put(84, 46, c.amber);
    c.vline(52, 84, DECK, 0x9CF3);
    c.hline(46, 58, 84, 0x9CF3);
    c.rect(56, 90, 70, DECK - 1, 0x7B60);
    c.outline(56, 90, 70, DECK - 1, c.dim);

    // Net drum and floats.
    c.rect(72, 92, 80, DECK - 1, 0x39C7);
    for (int x = 60; x <= 92; x += 6) {
        c.put(x, 105, 0xFD20);
    }
}

void drawSailing(Canvas& c) {
    drawHull(c, true, 0, 0, 0x8000, 0xEF7D, 0xCE79);
    c.hline(STERN, BOW_DECK + 6, 100, 0x02AA);

    // Low coachroof and cockpit.
    c.rect(80, 88, 116, DECK - 1, 0xE71C);
    c.outline(80, 88, 116, DECK - 1, c.dim);
    c.rect(84, 90, 112, 92, 0x18E3);

    // Mast, boom and rigging.
    c.vline(94, 34, deckY(94), c.highlight);
    c.hline(91, 97, 34, c.amber);
    c.hline(90, 98, 32, 0x8410);
    c.line(94, 34, 64, 98, c.dim);
    c.line(94, 34, 144, 98, c.dim);

    // Mainsail with a few seams.
    for (int y = 40; y <= 98; ++y) {
        const int right = 96 + (y - 40) * 48 / 58;
        c.hline(96, right, y, 0xEF7D);
        if ((y % 9) == 0) c.hline(96, right - 2, y, 0xCE79);
    }
    // Jib.
    for (int y = 42; y <= 98; ++y) {
        const int left = 92 - (y - 42) * 28 / 56;
        c.hline(left, 92, y, 0xE71C);
    }
    c.line(30, 104, 158, 104, c.dim);
}

void drawTugHull(Canvas& c) {
    const int stern = 52;
    const int bow_deck = 146;
    const int deck = 98;
    for (int y = deck - 4; y <= KEEL; ++y) {
        int left;
        int right;
        if (y < deck) {
            left = stern + (deck - y) * (bow_deck - stern) / 4;
            right = bow_deck + (y - (deck - 4)) * 6 / 4;
        } else if (y <= WATERLINE) {
            left = stern - (y - deck) * 3 / (KEEL - deck);
            right = bow_deck + 6 + (y - deck) * 6 / (WATERLINE - deck);
        } else {
            left = stern - 3 - (y - WATERLINE) / 2;
            right = bow_deck + 12 - (y - WATERLINE) * 10 /
                                         (KEEL - WATERLINE);
        }
        if (right <= left) continue;
        uint16_t color;
        if (y <= WATERLINE) {
            color = lerp565(0x2104, 0x0841,
                            static_cast<float>(y - (deck - 4)) /
                                static_cast<float>(WATERLINE - (deck - 4)));
        } else {
            color = (y <= WATERLINE + 1)
                        ? 0x8000
                        : lerp565(0x3800, 0x1800,
                                  static_cast<float>(y - WATERLINE) /
                                      static_cast<float>(KEEL - WATERLINE));
        }
        c.hline(left, right, y, color);
    }
    c.line(stern, deck, bow_deck, deck - 4, c.highlight);
    c.line(bow_deck, deck - 4, bow_deck + 6, deck, c.highlight);
    c.line(bow_deck + 6, deck, bow_deck + 12, WATERLINE, c.highlight);
    for (int x = stern + 8; x <= bow_deck - 6; x += 12) {
        c.vline(x, deck + 2, WATERLINE - 1, 0x2965);
    }
    for (int y = WATERLINE; y <= KEEL; ++y) {
        const int left = stern - 3 - (y - WATERLINE) / 2;
        const int right = bow_deck + 12 - (y - WATERLINE) * 10 /
                                             (KEEL - WATERLINE);
        const float depth = static_cast<float>(y - WATERLINE) /
                            static_cast<float>(KEEL - WATERLINE);
        const float alpha = 0.42f + 0.30f * depth;
        for (int x = left; x <= right; ++x) {
            c.blend(x, y, seaColorAt(y), alpha);
        }
    }
    c.hline(stern - 2, bow_deck + 12, WATERLINE, 0x3C9E);
}

void drawTug(Canvas& c) {
    drawTugHull(c);

    // Fenders along the working side.
    for (int x = 58; x <= 142; x += 10) {
        c.rect(x, WATERLINE - 6, x + 4, WATERLINE - 3, 0x2104);
    }

    // Wheelhouse and bridge.
    c.rect(80, 76, 122, 97, 0xD69A);
    c.outline(80, 76, 122, 97, c.dim);
    drawBridgeWindows(c, 83, 119, 82, 7);
    drawBridgeWindows(c, 83, 119, 90, 7);
    c.rect(78, 68, 126, 76, 0x4A49);
    c.outline(78, 68, 126, 76, c.dim);
    drawBridgeWindows(c, 81, 123, 71, 7);
    c.hline(78, 126, 68, c.highlight);

    // Funnel: black with a warm band.
    c.rect(96, 56, 110, 68, 0x2104);
    c.rect(97, 58, 109, 67, 0x7B60);
    c.hline(97, 109, 61, c.amber);
    c.hline(96, 110, 56, 0x0000);

    // Mast.
    c.vline(128, 44, 68, c.dim);
    c.hline(121, 135, 52, c.dim);
    c.put(128, 44, c.amber);

    // Towing winch at the stern.
    c.rect(56, 88, 74, 97, 0x39C7);
    c.outline(56, 88, 74, 97, c.dim);
    c.vline(60, 84, 88, c.dim);
    c.vline(70, 84, 88, c.dim);
    c.hline(60, 70, 84, 0x9CF3);
}

void drawSmallBoat(Canvas& c) {
    // Planing hull: white topsides, navy sheer stripe, red boot top.
    const int deck = 94;
    for (int y = deck; y <= KEEL; ++y) {
        int left;
        int right;
        if (y <= WATERLINE) {
            left = 64 - (y - deck) / 4;
            right = 138 + (y - deck) * 12 / (WATERLINE - deck);
        } else {
            left = 61 - (y - WATERLINE) / 2;
            right = 150 - (y - WATERLINE) * 3;
        }
        uint16_t color;
        if (y < deck + 7) {
            color = lerp565(0xEF7D, 0xC618,
                            (y - deck) / 7.0f);
        } else if (y <= WATERLINE) {
            color = lerp565(0x2966, 0x18C3,
                            (y - deck - 7) /
                                static_cast<float>(WATERLINE - deck - 7));
        } else {
            color = lerp565(0x8000, 0x3800,
                            (y - WATERLINE) /
                                static_cast<float>(KEEL - WATERLINE));
        }
        c.hline(left, right, y, color);
    }
    c.line(64, deck, 138, deck, c.highlight);
    c.line(138, deck, 150, WATERLINE, c.highlight);
    c.line(150, WATERLINE, 147, KEEL, c.highlight);
    c.line(64, deck, 61, KEEL, c.highlight);
    for (int y = WATERLINE; y <= KEEL; ++y) {
        const int left = 61 - (y - WATERLINE) / 2;
        const int right = 150 - (y - WATERLINE) * 3;
        const float depth = static_cast<float>(y - WATERLINE) /
                            static_cast<float>(KEEL - WATERLINE);
        const float alpha = 0.42f + 0.30f * depth;
        for (int x = left; x <= right; ++x) {
            c.blend(x, y, seaColorAt(y), alpha);
        }
    }
    c.hline(58, 152, WATERLINE, 0x3C9E);
    c.hline(66, 146, deck + 8, 0x02AA);

    // Cabin and raked windscreen.
    c.rect(84, 78, 118, deck - 1, 0xDEFB);
    c.outline(84, 78, 118, deck - 1, c.dim);
    c.rect(87, 81, 115, 87, 0x18E3);
    c.hline(89, 113, 82, 0x39E7);
    c.hline(84, 118, 78, c.highlight);
    c.rect(86, 90, 116, deck - 1, 0x39C7);

    // Console, antenna and outboard.
    c.rect(120, 86, 132, deck - 1, 0x39C7);
    c.outline(120, 86, 132, deck - 1, c.dim);
    c.vline(127, 66, 86, c.dim);
    c.hline(123, 131, 72, c.dim);
    c.put(127, 66, c.amber);
    c.rect(56, 100, 64, 112, 0x2965);
    c.hline(56, 64, 100, c.highlight);

    // Bow rail, fenders and wake.
    c.line(134, 90, 144, 95, c.highlight);
    c.line(138, 90, 148, 92, 0x9CF3);
    c.line(120, 92, 138, 92, 0x9CF3);
    c.rect(68, 100, 72, 104, 0x2104);
    for (int i = 0; i < 5; ++i) {
        c.hline(46 - i * 8, 54 - i * 8, 108 + (i & 1), 0x2A8A);
    }
}

void drawAton(Canvas& c) {
    // Buoy hull at the waterline.
    for (int y = 96; y <= WATERLINE; ++y) {
        const float t = static_cast<float>(y - 96) /
                        static_cast<float>(WATERLINE - 96);
        const int half = 12 + static_cast<int>(t * 4.0f);
        c.hline(CX - half, CX + half, y, lerp565(0xD800, 0x8800, t));
    }
    c.outline(CX - 12, 96, CX + 12, WATERLINE, c.dim);
    for (int y = 104; y <= 108; ++y) {
        c.hline(CX - 14, CX + 14, y, 0xD69A);
    }
    // Lattice tower and top mark.
    c.rect(CX - 5, 70, CX + 5, 96, 0x18E3);
    c.outline(CX - 5, 70, CX + 5, 96, c.dim);
    c.hline(CX - 3, CX + 3, 74, c.amber);
    c.hline(CX - 3, CX + 3, 84, c.amber);
    c.line(CX, 70, CX, 60, c.dim);
    c.line(CX - 5, 62, CX + 5, 66, c.amber);
    c.line(CX + 5, 62, CX - 5, 66, c.amber);
    c.put(CX, 58, c.amber);

    // Reflection.
    for (int y = WATERLINE + 2; y <= WATERLINE + 14; y += 2) {
        c.hline(CX - 8 + (y & 3), CX + 8 - (y & 3), y, 0x2A8A);
    }
}

void drawBase(Canvas& c) {
    // Quay edge and bollards.
    c.rect(18, 104, 182, 112, 0x5AEB);
    c.hline(18, 182, 104, 0x8C71);
    c.hline(18, 182, 112, 0x2965);
    for (int x = 24; x <= 176; x += 14) {
        c.rect(x, 106, x + 2, 109, 0x2104);
    }

    // Control building.
    c.rect(56, 88, 118, 104, 0xBDF7);
    c.outline(56, 88, 118, 104, c.dim);
    drawBridgeWindows(c, 60, 112, 94, 8);
    c.hline(56, 118, 88, c.highlight);
    c.rect(60, 82, 74, 88, 0x39C7);

    // Lattice antenna tower.
    c.line(126, 104, 132, 40, c.dim);
    c.line(140, 104, 134, 40, c.dim);
    for (int y = 48; y < 104; y += 10) {
        c.line(129 + (104 - y) / 9, y, 137 - (104 - y) / 9, y + 8, c.dim);
        c.hline(129 + (104 - y) / 9, 137 - (104 - y) / 9, y, c.dim);
    }
    c.hline(124, 142, 40, c.highlight);
    c.vline(133, 28, 40, c.dim);
    c.put(133, 27, c.amber);

    // Emitted arcs.
    for (int r = 6; r <= 14; r += 4) {
        for (int deg = 200; deg <= 340; deg += 4) {
            const float a = deg * static_cast<float>(kPi) / 180.0f;
            c.put(133 + static_cast<int>(std::lround(std::sin(a) * r)),
                  27 - static_cast<int>(std::lround(std::cos(a) * r)),
                  0x2C6B);
        }
    }
}

void drawGeneric(Canvas& c) {
    drawHull(c, false, 0x3186, 0x18E3, 0x8000, 0, 0);
    c.rect(40, 72, 72, DECK - 1, 0xBDF7);
    c.outline(40, 72, 72, DECK - 1, c.dim);
    drawBridgeWindows(c, 43, 69, 78, 7);
    c.rect(36, 64, 76, 72, 0x4A49);
    c.outline(36, 64, 76, 72, c.dim);
    drawBridgeWindows(c, 39, 73, 67, 7);
    c.rect(48, 50, 58, 64, 0x2104);
    c.hline(49, 57, 54, c.amber);
    c.vline(62, 42, 64, c.dim);
    c.put(62, 42, c.amber);
    c.rect(88, 78, 126, DECK - 1, 0x39C7);
    c.outline(88, 78, 126, DECK - 1, c.dim);
    c.hline(90, 124, 84, 0x7B60);
    c.vline(146, 56, deckY(146), c.dim);
}

}  // namespace

void drawDossierPortrait(Framebuffer& fb, const ais::VesselState* vessel,
                         uint16_t accent, uint16_t dim,
                         uint16_t highlight, uint16_t amber) {
    // Sky and sea, clipped by the porthole.
    for (int y = CY - R; y <= CY + R; ++y) {
        const int dy = y - CY;
        const int half = static_cast<int>(
            std::sqrt(static_cast<float>(R * R - dy * dy)));
        for (int x = CX - half; x <= CX + half; ++x) {
            uint16_t color;
            if (y < HORIZON) {
                const float t = static_cast<float>(y - (CY - R)) /
                                static_cast<float>(HORIZON - (CY - R));
                color = lerp565(0x0841, 0x5DDF, t * t);
            } else {
                const float t = static_cast<float>(y - HORIZON) /
                                static_cast<float>((CY + R) - HORIZON);
                color = lerp565(0x0ACE, 0x0147, t);
            }
            fb.drawPixel(x, y, color);
        }
    }

    // Warm haze just above the horizon.
    for (int y = HORIZON - 12; y < HORIZON + 2; ++y) {
        const int dy = y - CY;
        const int half = static_cast<int>(
            std::sqrt(static_cast<float>(R * R - dy * dy)));
        for (int x = CX - half; x <= CX + half; ++x) {
            fb.blendPixel(x, y, 0xFE80, 0.035f);
        }
    }

    // Horizon line and sea texture.
    for (int x = CX - R; x <= CX + R; ++x) {
        fb.drawPixel(x, HORIZON, 0x2D3F);
        fb.drawPixel(x, HORIZON + 1, 0x1A6B);
    }
    for (int y = HORIZON + 6; y <= CY + R - 6; y += 6) {
        const int dy = y - CY;
        const int half = static_cast<int>(
            std::sqrt(static_cast<float>(R * R - dy * dy)));
        for (int x = CX - half + 4; x < CX + half - 4; ++x) {
            if (((x + y * 2) / 6) % 4 == 0) {
                fb.blendPixel(x, y, 0x0C6F, 0.28f);
            }
        }
    }

    Canvas canvas{fb, accent, dim, highlight, amber};

    const ais::VesselState* subject = vessel;

    if (subject != nullptr) {
        int type = subject->ship_type;
        bool base_station = subject->base_station;
        bool aton = subject->aton;
        bool class_b = subject->class_b;
        if (base_station) {
            drawBase(canvas);
        } else if (aton) {
            drawAton(canvas);
        } else if (type == 36 || type == 37) {
            drawSailing(canvas);
        } else if (class_b) {
            drawSmallBoat(canvas);
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
        } else {
            drawGeneric(canvas);
        }
    }

    // Soft immersion band where the hull meets the sea.
    for (int y = WATERLINE - 2; y <= WATERLINE + 2; ++y) {
        for (int x = STERN - 8; x <= BOW_DECK + 12; ++x) {
            canvas.blend(x, y, 0x0147, 0.22f);
        }
    }

    // Porthole glass: a soft highlight along the upper-left rim.
    for (int pass = 0; pass < 2; ++pass) {
        const int radius = (pass == 0) ? R - 4 : R - 10;
        const float alpha = (pass == 0) ? 0.05f : 0.08f;
        for (int deg = 198; deg <= 252; deg += 2) {
            const float a = deg * static_cast<float>(kPi) / 180.0f;
            const int x = CX + static_cast<int>(std::lround(std::sin(a) * radius));
            const int y = CY - static_cast<int>(std::lround(std::cos(a) * radius));
            fb.blendPixel(x, y, 0xFFFF, alpha);
        }
    }

    // Porthole bezel, rivets and compass ticks.
    for (int r = R; r <= R_OUTER; ++r) {
        uint16_t ring = 0x2124;
        if (r == R) ring = accent;
        else if (r == R + 1) ring = 0x18C3;
        else if (r == R_OUTER) ring = dim;
        else if (r == R + 5) ring = 0x2965;
        circleOutline(fb, CX, CY, r, ring);
    }
    for (int deg = 0; deg < 360; deg += 10) {
        const float a = deg * static_cast<float>(kPi) / 180.0f;
        const int inner = (deg % 30 == 0) ? R - 8 : R - 4;
        fb.drawLine(CX + static_cast<int>(std::lround(std::sin(a) * inner)),
                    CY - static_cast<int>(std::lround(std::cos(a) * inner)),
                    CX + static_cast<int>(std::lround(std::sin(a) * (R - 2))),
                    CY - static_cast<int>(std::lround(std::cos(a) * (R - 2))),
                    (deg % 30 == 0) ? dim : 0x2965);
    }
    for (int deg = 0; deg < 360; deg += 30) {
        const float a = deg * static_cast<float>(kPi) / 180.0f;
        const int rx = CX + static_cast<int>(std::lround(std::sin(a) * 81));
        const int ry = CY - static_cast<int>(std::lround(std::cos(a) * 81));
        fb.drawPixel(rx, ry, 0x9E9F);
        fb.drawPixel(rx + 1, ry, 0x4208);
    }

    // Cardinal labels, tucked inside the tick ring.
    const char* cardinals[4] = {"N", "E", "S", "W"};
    const int cardinal_deg[4] = {0, 90, 180, 270};
    for (int i = 0; i < 4; ++i) {
        const float a = cardinal_deg[i] * static_cast<float>(kPi) / 180.0f;
        const int x = CX + static_cast<int>(std::lround(std::sin(a) * 64)) - 3;
        const int y = CY - static_cast<int>(std::lround(std::cos(a) * 64)) - 4;
        drawText6(fb, x, y, cardinals[i],
                  i == 0 ? amber : static_cast<uint16_t>(0x8410));
    }

    // Heading bug on the bezel.
    const float heading = subject == nullptr ? 0.0f
                        : (subject->heading >= 0 ? subject->heading
                        : (subject->cog >= 0 ? subject->cog : 0.0f));
    const float a = heading * static_cast<float>(kPi) / 180.0f;
    const int sx = CX + static_cast<int>(std::lround(std::sin(a) * 68));
    const int sy = CY - static_cast<int>(std::lround(std::cos(a) * 68));
    const int ex = CX + static_cast<int>(std::lround(std::sin(a) * 74));
    const int ey = CY - static_cast<int>(std::lround(std::cos(a) * 74));
    fb.drawLine(sx, sy, ex, ey, amber);
    fb.drawLine(ex, ey,
                CX + static_cast<int>(std::lround(std::sin(a + 0.10f) * 69)),
                CY - static_cast<int>(std::lround(std::cos(a + 0.10f) * 69)),
                amber);
    fb.drawLine(ex, ey,
                CX + static_cast<int>(std::lround(std::sin(a - 0.10f) * 69)),
                CY - static_cast<int>(std::lround(std::cos(a - 0.10f) * 69)),
                amber);

    // Heading plate at the bottom of the porthole.
    char text[24];
    if (subject != nullptr) {
        std::snprintf(text, sizeof(text), "HDG %03.0f", heading);
    } else {
        std::snprintf(text, sizeof(text), "NO TARGET");
    }
    fb.fillRect(CX - 27, 132, 54, 11, 0x0841);
    fb.drawRect(CX - 27, 132, 54, 11, 0x2965);
    drawText6(fb, CX - 23, 133, text, amber);
}

}  // namespace ais_ui
