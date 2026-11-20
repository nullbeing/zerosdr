#include "ui/adsb_ui_renderer.h"
#include "font.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

// Define static constants
const int ADSBUIRenderer::STATUS_BAR_Y;
const int ADSBUIRenderer::STATUS_BAR_H;
const int ADSBUIRenderer::HEADER_Y;
const int ADSBUIRenderer::HEADER_H;
const int ADSBUIRenderer::LIST_Y;
const int ADSBUIRenderer::LIST_H;
const int ADSBUIRenderer::STATS_BAR_Y;
const int ADSBUIRenderer::STATS_BAR_H;
const int ADSBUIRenderer::ROW_HEIGHT;
const int ADSBUIRenderer::MAX_VISIBLE_ROWS;
const int ADSBUIRenderer::COL_ICAO_X;
const int ADSBUIRenderer::COL_ICAO_W;
const int ADSBUIRenderer::COL_CALL_X;
const int ADSBUIRenderer::COL_CALL_W;
const int ADSBUIRenderer::COL_ALT_X;
const int ADSBUIRenderer::COL_ALT_W;
const int ADSBUIRenderer::COL_SPD_X;
const int ADSBUIRenderer::COL_SPD_W;
const int ADSBUIRenderer::COL_HDG_X;
const int ADSBUIRenderer::COL_HDG_W;
const int ADSBUIRenderer::COL_DIST_X;
const int ADSBUIRenderer::COL_DIST_W;
const int ADSBUIRenderer::COL_SEEN_X;
const int ADSBUIRenderer::COL_SEEN_W;

ADSBUIRenderer::ADSBUIRenderer(Framebuffer& fb)
    : BaseUIRenderer(fb) {
}

ADSBUIRenderer::~ADSBUIRenderer() {
}

// 8x16 font overrides for ADSB page
void ADSBUIRenderer::drawChar(int x, int y, char c, uint16_t color) {
    if (c < 0x20 || c > 0x7E) return;

    const uint8_t* glyph = font8x16[c - 0x20];

    for (int row = 0; row < FONT_8X16_HEIGHT; row++) {
        uint8_t line = glyph[row];
        for (int col = 0; col < FONT_8X16_WIDTH; col++) {
            if (line & (0x80 >> col)) {
                fb_.drawPixel(x + col, y + row, color);
            }
        }
    }
}

void ADSBUIRenderer::drawText(int x, int y, const char* text, uint16_t color) {
    if (!text) return;

    int cursor_x = x;
    while (*text) {
        drawChar(cursor_x, y, *text, color);
        cursor_x += FONT_8X16_WIDTH + 1; // 8px char + 1px spacing
        text++;
    }
}

void ADSBUIRenderer::drawSmallText(int x, int y, const char* text, uint16_t color) {
    if (!text) return;
    int cursor_x = x;
    while (*text) {
        if (*text >= 0x20 && *text <= 0x7E) {
            const uint8_t* glyph = font6x8[*text - 0x20];
            for (int row = 0; row < FONT_6X8_HEIGHT; row++) {
                uint8_t line = glyph[row];
                for (int col = 0; col < FONT_6X8_WIDTH; col++) {
                    if (line & (0x80 >> col)) {
                        fb_.drawPixel(cursor_x + col, y + row, color);
                    }
                }
            }
        }
        cursor_x += FONT_6X8_WIDTH + 1;
        text++;
    }
}

void ADSBUIRenderer::drawTextBg(int x, int y, const char* text, uint16_t fg, uint16_t bg) {
    if (!text) return;

    int text_len = 0;
    const char* p = text;
    while (*p++) text_len++;

    fb_.fillRect(x, y, text_len * (FONT_8X16_WIDTH + 1), FONT_8X16_HEIGHT, bg);
    drawText(x, y, text, fg);
}

int ADSBUIRenderer::getTextWidth(const char* text) const {
    if (!text) return 0;
    return strlen(text) * (FONT_8X16_WIDTH + 1);
}

void ADSBUIRenderer::ensureUiFont() const {
    if (!ui_font_loaded_) {
        ui_font_loaded_ = true;
        loadDefaultUiFont(ui_font_, 12);
    }
}

void ADSBUIRenderer::renderStatusBar(int aircraft_count,
                                     uint32_t message_count,
                                     float message_rate) {
    // Clear status bar area with blue background
    fb_.fillRect(0, STATUS_BAR_Y, 320, STATUS_BAR_H, COLOR_ADSB_STATUSBAR_BG);

    // Format status string: "AC:5 Msg:1234 45/s 1090MHz"
    char status[64];
    snprintf(status, sizeof(status), "AC:%d Msg:%u %.0f/s 1090MHz",
             aircraft_count, message_count, message_rate);

    ensureUiFont();
    if (ui_font_.valid()) {
        ui_font_.draw(fb_, 2, STATUS_BAR_Y + 12, status,
                      COLOR_ADSB_STATUSBAR_TEXT);
    } else {
        drawText(2, STATUS_BAR_Y + 2, status, COLOR_ADSB_STATUSBAR_TEXT);
    }
}

void ADSBUIRenderer::renderColumnHeaders() {
    // Clear header area
    fb_.fillRect(0, HEADER_Y, 320, HEADER_H, COLOR_BG_DARK);

    // Draw separator line
    drawHLine(0, HEADER_Y + HEADER_H - 1, 320, COLOR_GRID);

    // Draw column headers
    drawText(COL_ICAO_X, HEADER_Y + 1, "ICAO", COLOR_TEXT);
    drawText(COL_CALL_X, HEADER_Y + 1, "Call", COLOR_TEXT);
    drawText(COL_ALT_X, HEADER_Y + 1, "Alt", COLOR_TEXT);
    drawText(COL_SPD_X, HEADER_Y + 1, "Spd", COLOR_TEXT);
    drawText(COL_HDG_X, HEADER_Y + 1, "Hdg", COLOR_TEXT);
    drawText(COL_DIST_X, HEADER_Y + 1, "Dist", COLOR_TEXT);
    drawText(COL_SEEN_X, HEADER_Y + 1, "Seen", COLOR_TEXT);
}

void ADSBUIRenderer::renderAircraftList(const std::vector<AircraftState>& aircraft,
                                        int selected_index,
                                        int scroll_offset) {
    // Clear list area
    fb_.fillRect(0, LIST_Y, 320, LIST_H, COLOR_BG_DARK);

    // Calculate visible range
    int visible_count = std::min((int)aircraft.size() - scroll_offset, MAX_VISIBLE_ROWS);

    for (int i = 0; i < visible_count; i++) {
        int aircraft_idx = scroll_offset + i;
        if (aircraft_idx >= (int)aircraft.size()) break;

        const AircraftState& ac = aircraft[aircraft_idx];
        int row_y = LIST_Y + (i * ROW_HEIGHT);
        bool is_selected = (aircraft_idx == selected_index);

        // Highlight selected row
        if (is_selected) {
            fb_.fillRect(0, row_y, 320, ROW_HEIGHT, COLOR_SELECTED);
        }

        uint16_t text_color = is_selected ? COLOR_TEXT_BRIGHT : COLOR_TEXT;

        // Format and draw each column
        char buffer[16];

        // ICAO address (6 hex digits)
        snprintf(buffer, sizeof(buffer), "%06X", ac.icao_addr);
        drawText(COL_ICAO_X, row_y + 2, buffer, text_color);

        // Callsign (7 chaned) with white background and black text
        if (ac.callsign[0] != '\0') {
            // Calculate actual callsign length (without trailing spaces)
            int callsign_len = strlen(ac.callsign);
            while (callsign_len > 0 && ac.callsign[callsign_len - 1] == ' ') {
                callsign_len--;
            }

            if (callsign_len > 0) {
                // Draw white background only for actual callsign characters
                fb_.fillRect(COL_CALL_X, row_y + 2, callsign_len * 9, 16, COLOR_WHITE);
                // Draw callsign without padding
                char callsign_buf[9];
                snprintf(callsign_buf, sizeof(callsign_buf), "%.*s", callsign_len, ac.callsign);
                drawText(COL_CALL_X, row_y + 2, callsign_buf, COLOR_BLACK);
            } else {
                snprintf(buffer, sizeof(buffer), "-------");
                drawText(COL_CALL_X, row_y + 2, buffer, text_color);
            }
        } else {
            snprintf(buffer, sizeof(buffer), "-------");
            drawText(COL_CALL_X, row_y + 2, buffer, text_color);
        }

        // Altitude (5 chars, right-aligned)
        if (ac.position_valid && ac.altitude > 0) {
            snprintf(buffer, sizeof(buffer), "%5d", ac.altitude);
        } else {
            snprintf(buffer, sizeof(buffer), "    -");
        }
        drawText(COL_ALT_X, row_y + 2, buffer, text_color);

        // Speed (3 chars, right-aligned)
        if (ac.velocity_valid && ac.speed > 0) {
            snprintf(buffer, sizeof(buffer), "%3d", ac.speed);
        } else {
            snprintf(buffer, sizeof(buffer), "  -");
        }
        drawText(COL_SPD_X, row_y + 2, buffer, text_color);

        // Heading (3 chars, right-aligned)
        if (ac.velocity_valid && ac.heading >= 0) {
            snprintf(buffer, sizeof(buffer), "%03d", ac.heading);
        } else {
            snprintf(buffer, sizeof(buffer), "  -");
        }
        drawText(COL_HDG_X, row_y + 2, buffer, text_color);

        // Distance (5 chars with unit)
        formatDistance(ac.distance, buffer);
        drawText(COL_DIST_X, row_y + 2, buffer, text_color);

        // Time since last seen (4 chars)
        time_t now = time(nullptr);
        time_t elapsed = now - ac.last_seen;
        formatTimeSince(elapsed, buffer);
        drawText(COL_SEEN_X, row_y + 2, buffer, text_color);
    }

    // Draw separator line at bottom of list
    drawHLine(0, LIST_Y + LIST_H - 1, 320, COLOR_GRID);
}

void ADSBUIRenderer::renderStatistics(uint32_t total_messages,
                                      uint32_t crc_errors,
                                      float max_range) {
    // Clear statistics area with blue background
    fb_.fillRect(0, STATS_BAR_Y, 320, STATS_BAR_H, COLOR_ADSB_STATUSBAR_BG);

    // Format statistics string: "Total:1234 CRC:12 Range:50km"
    char stats[64];
    snprintf(stats, sizeof(stats), "Total:%u CRC:%u Range:%.0fkm",
             total_messages, crc_errors, max_range);

    ensureUiFont();
    if (ui_font_.valid()) {
        ui_font_.draw(fb_, 2, STATS_BAR_Y + 14, stats,
                      COLOR_ADSB_STATUSBAR_TEXT);
    } else {
        drawText(2, STATS_BAR_Y + 2, stats, COLOR_ADSB_STATUSBAR_TEXT);
    }
}

void ADSBUIRenderer::renderAircraftDetail(const AircraftState& aircraft,
                                          int scroll_offset,
                                          int aircraft_count,
                                          uint32_t message_count,
                                          float message_rate,
                                          float gain,
                                          bool square) {
    fb_.clear(COLOR_BG_DARK);

    // Left: the full-height PFD. Right: the aircraft text column. The bottom
    // status bar is intentionally gone so the PFD can use the whole height.
    const int PFD_W = 200;
    const int PFD_H = fb_.getHeight();
    const int INFO_X = PFD_W;
    const int INFO_Y = 0;
    const int INFO_W = fb_.getWidth() - PFD_W;
    const int INFO_H = PFD_H;

    renderPFD(aircraft, 0, 0, PFD_W, PFD_H, square);

    std::vector<std::string> lines;
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "ICAO: %06X", aircraft.icao_addr);
    lines.push_back(std::string(buffer));

    if (aircraft.callsign[0] != '\0') {
        snprintf(buffer, sizeof(buffer), "Call: %s", aircraft.callsign);
        lines.push_back(std::string(buffer));
    }

    if (aircraft.position_valid) {
        snprintf(buffer, sizeof(buffer), "Lat: %.6f", aircraft.latitude);
        lines.push_back(std::string(buffer));
        snprintf(buffer, sizeof(buffer), "Lon: %.6f", aircraft.longitude);
        lines.push_back(std::string(buffer));
        snprintf(buffer, sizeof(buffer), "Alt: %d ft", aircraft.altitude);
        lines.push_back(std::string(buffer));
    }

    if (aircraft.velocity_valid) {
        snprintf(buffer, sizeof(buffer), "Spd: %d kt", aircraft.speed);
        lines.push_back(std::string(buffer));
        snprintf(buffer, sizeof(buffer), "Hdg: %d deg", aircraft.heading);
        lines.push_back(std::string(buffer));
        snprintf(buffer, sizeof(buffer), "V/S: %d fpm", aircraft.vertical_rate);
        lines.push_back(std::string(buffer));
    }

    if (aircraft.distance > 0) {
        snprintf(buffer, sizeof(buffer), "Dist: %.1f km", aircraft.distance);
        lines.push_back(std::string(buffer));
        snprintf(buffer, sizeof(buffer), "Bear: %.0f deg", aircraft.bearing);
        lines.push_back(std::string(buffer));
    }

    snprintf(buffer, sizeof(buffer), "Msgs: %u", aircraft.message_count);
    lines.push_back(std::string(buffer));

    time_t now = time(nullptr);
    time_t elapsed = now - aircraft.last_seen;
    snprintf(buffer, sizeof(buffer), "Seen: %ld s", (long)elapsed);
    lines.push_back(std::string(buffer));

    fb_.fillRect(INFO_X, INFO_Y, INFO_W, INFO_H, COLOR_BG_DARK);

    ensureUiFont();
    const int LINE_HEIGHT = 14;
    int y = INFO_Y + 2 - scroll_offset;

    for (size_t i = 0; i < lines.size(); i++) {
        if (y + LINE_HEIGHT > INFO_Y && y < INFO_H) {
            if (ui_font_.valid()) {
                ui_font_.draw(fb_, INFO_X + 5, y + 11, lines[i].c_str(),
                              COLOR_TEXT_BRIGHT);
            } else {
                drawText(INFO_X + 5, y, lines[i].c_str(), COLOR_TEXT_BRIGHT);
            }
        }
        y += LINE_HEIGHT;
    }
}

void ADSBUIRenderer::formatTimeSince(time_t seconds, char* buffer) {
    if (seconds < 60) {
        snprintf(buffer, 5, "%2lds", (long)seconds);
    } else if (seconds < 3600) {
        snprintf(buffer, 5, "%2ldm", (long)(seconds / 60));
    } else {
        snprintf(buffer, 5, "%2ldh", (long)(seconds / 3600));
    }
}

void ADSBUIRenderer::formatDistance(float distance, char* buffer) {
    if (distance < 0.1f) {
        snprintf(buffer, 6, "   -");
    } else if (distance < 10.0f) {
        snprintf(buffer, 6, "%4.1fk", distance);
    } else if (distance < 100.0f) {
        snprintf(buffer, 6, "%4.0fk", distance);
    } else {
        snprintf(buffer, 6, " >99k");
    }
}

// PFD rendering implementation

int ADSBUIRenderer::roundedSquareHalfWidth(int dy, int half, int corner) const {
    const int ay = std::abs(dy);
    if (ay <= half - corner) return half;
    const int d = ay - (half - corner);
    const int rr = corner * corner - d * d;
    if (rr <= 0) return std::max(0, half - corner);
    return (half - corner) + (int)lroundf(std::sqrt((float)rr));
}

void ADSBUIRenderer::drawRoundedSquareOutline(int cx, int cy, int half,
                                              int corner, uint16_t color) {
    if (half <= 0) return;
    for (int dy = -half; dy <= half; ++dy) {
        const int hw = roundedSquareHalfWidth(dy, half, corner);
        fb_.drawPixel(cx - hw, cy + dy, color);
        fb_.drawPixel(cx + hw, cy + dy, color);
    }
    const int edge = std::max(0, half - corner);
    for (int dx = -edge; dx <= edge; ++dx) {
        fb_.drawPixel(cx + dx, cy - half, color);
        fb_.drawPixel(cx + dx, cy + half, color);
    }
}

void ADSBUIRenderer::drawCircleApprox(int cx, int cy, int radius, uint16_t color) {
    // Bresenham's circle algorithm
    int x = 0;
    int y = radius;
    int d = 3 - 2 * radius;
    
    while (x <= y) {
        // Draw 8 octants
        fb_.drawPixel(cx + x, cy + y, color);
        fb_.drawPixel(cx - x, cy + y, color);
        fb_.drawPixel(cx + x, cy - y, color);
        fb_.drawPixel(cx - x, cy - y, color);
        fb_.drawPixel(cx + y, cy + x, color);
        fb_.drawPixel(cx - y, cy + x, color);
        fb_.drawPixel(cx + y, cy - x, color);
        fb_.drawPixel(cx - y, cy - x, color);

        if (d < 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

float ADSBUIRenderer::calculatePitchAngle(const AircraftState& aircraft) {
    // Need valid velocity data and reasonable speed
    if (!aircraft.velocity_valid || aircraft.speed < 50) {
        return 0.0f;
    }

    // Convert vertical rate from feet/min to feet/sec
    float vr_fps = aircraft.vertical_rate / 60.0f;

    // Convert ground speed from knots to feet/sec
    // 1 knot = 1.68781 feet/sec
    float gs_fps = aircraft.speed * 1.68781f;

    // Calculate pitch angle (flight path angle)
    float pitch_rad = atan2f(vr_fps, gs_fps);
    float pitch_deg = pitch_rad * 180.0f / M_PI;

    // Clamp to reasonable display range
    if (pitch_deg > 15.0f) pitch_deg = 15.0f;
    if (pitch_deg < -15.0f) pitch_deg = -15.0f;

    return pitch_deg;
}

namespace {

// Linear blend between two RGB565 colours. The PFD only evaluates this once
// per row for the sky/ground gradients, so it stays cheap on the Pi Zero.
uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    const int ar = (a >> 11) & 0x1F;
    const int ag = (a >> 5) & 0x3F;
    const int ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F;
    const int bg = (b >> 5) & 0x3F;
    const int bb = b & 0x1F;
    const int r = ar + (int)((br - ar) * t + 0.5f);
    const int g = ag + (int)((bg - ag) * t + 0.5f);
    const int bl = ab + (int)((bb - ab) * t + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

}  // namespace

void ADSBUIRenderer::drawSegDigit(int x, int y, int digit, int clip_top,
                                  int clip_bottom, uint16_t color) {
    static const uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66,
                                     0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const uint8_t mask = (digit >= 0 && digit <= 9) ? kSeg[digit] : 0;

    auto seg = [&](int sx, int sy, int sw, int sh, uint16_t color) {
        const int y0 = std::max(sy, clip_top);
        const int y1 = std::min(sy + sh - 1, clip_bottom);
        if (y1 >= y0) fb_.fillRect(sx, y0, sw, y1 - y0 + 1, color);
    };

    auto draw_mask = [&](uint8_t m, uint16_t color) {
        if (m & 0x01) seg(x + 1, y, 4, 2, color);       // a
        if (m & 0x02) seg(x + 4, y + 2, 2, 5, color);   // b
        if (m & 0x04) seg(x + 4, y + 9, 2, 5, color);   // c
        if (m & 0x08) seg(x + 1, y + 14, 4, 2, color);  // d
        if (m & 0x10) seg(x, y + 9, 2, 5, color);       // e
        if (m & 0x20) seg(x, y + 2, 2, 5, color);       // f
        if (m & 0x40) seg(x + 1, y + 7, 4, 2, color);   // g
    };

    draw_mask(mask, color);
}

void ADSBUIRenderer::drawReadout(int x, int y, int w, int h, const char* text,
                                 bool right_aligned) {
    const int len = (int)strlen(text);
    if (len == 0) return;

    // Mask the scale behind the digits with the panel face: no frame, but
    // the ticks and labels do not bleed through the numbers.
    fb_.fillRect(x, y, w, h, COLOR_PFD_PANEL);

    const int pitch = 7;
    const int text_w = len * pitch;
    int tx = right_aligned ? (x + w - 1 - text_w) : (x + 1);
    if (tx < x + 1) tx = x + 1;
    const int ty = y + (h - 16) / 2;

    for (int i = 0; i < len; ++i) {
        const char ch = text[i];
        const int digit = (ch >= '0' && ch <= '9') ? ch - '0' : -1;
        drawSegDigit(tx + i * pitch, ty, digit, ty, ty + 15);
    }
}

void ADSBUIRenderer::drawArtificialHorizon(int cx, int cy, int radius,
                                           float pitch_angle, float bank_angle,
                                           bool square) {
    const float pixels_per_degree = 2.5f;

    // Bank rotates the whole world behind the fixed aircraft symbol.
    float bank = bank_angle;
    if (bank > 45.0f) bank = 45.0f;
    if (bank < -45.0f) bank = -45.0f;

    const float phi = bank * (float)M_PI / 180.0f;
    const float s = std::sin(phi);
    const float c = std::cos(phi);

    // Positive pitch (nose up) moves the horizon down behind the fixed
    // aircraft symbol.
    int y_offset = (int)lroundf(pitch_angle * pixels_per_degree);
    const int max_offset = radius - 6;
    if (y_offset > max_offset) y_offset = max_offset;
    if (y_offset < -max_offset) y_offset = -max_offset;
    const int horizon_y = cy + y_offset;

    const int top = cy - radius;
    const int bottom = cy + radius;
    const int sky_span = std::max(1, horizon_y - top);
    const int ground_span = std::max(1, bottom - horizon_y);

    // Fill the ball row by row. Each row is split where the tilted horizon
    // crosses it, which keeps the gradient cheap (one colour per row/side)
    // while the bank rotates the sky/ground boundary.
    for (int y = top; y <= bottom; ++y) {
        const int dy = y - cy;
        const int dx = square
                           ? roundedSquareHalfWidth(dy, radius, 14)
                           : (int)lroundf(std::sqrt(
                                 (float)(radius * radius - dy * dy)));
        const int x_left = cx - dx;
        const int x_right = cx + dx;

        float sky_t = (float)(y - top) / (float)sky_span;
        float ground_t = (float)(y - horizon_y) / (float)ground_span;
        sky_t = std::max(0.0f, std::min(1.0f, sky_t));
        ground_t = std::max(0.0f, std::min(1.0f, ground_t));
        const uint16_t sky_color =
            lerp565(COLOR_PFD_SKY_TOP, COLOR_PFD_SKY_LOW, sky_t);
        const uint16_t ground_color =
            lerp565(COLOR_PFD_GROUND_TOP, COLOR_PFD_GROUND_LOW, ground_t);

        if (std::fabs(s) < 0.005f) {
            fb_.drawLine(x_left, y, x_right, y,
                         (y < horizon_y) ? sky_color : ground_color);
            continue;
        }

        // x where the horizon line crosses this row. With a right bank the
        // sky lies to the right of that crossing.
        const int hx =
            (int)lroundf((float)cx + (float)(y - horizon_y) * c / s);
        if (hx <= x_left) {
            fb_.drawLine(x_left, y, x_right, y,
                         (s > 0.0f) ? sky_color : ground_color);
        } else if (hx >= x_right) {
            fb_.drawLine(x_left, y, x_right, y,
                         (s > 0.0f) ? ground_color : sky_color);
        } else if (s > 0.0f) {
            fb_.drawLine(x_left, y, hx - 1, y, ground_color);
            fb_.drawLine(hx, y, x_right, y, sky_color);
        } else {
            fb_.drawLine(x_left, y, hx - 1, y, sky_color);
            fb_.drawLine(hx, y, x_right, y, ground_color);
        }
    }

    // Rotated horizon bar: a two-pixel white core with dark lips, clipped to
    // the ball so it reads over both the pale sky and the tan ground.
    const float ux = c;
    const float uy = s;
    const float nx = -s;   // normal, positive towards the ground
    const float ny = c;

    auto clip_to_ball = [&](float ox, float oy, int& ax, int& ay, int& bx,
                            int& by) {
        const float px = ox - (float)cx;
        const float py = oy - (float)cy;
        const float b = px * ux + py * uy;
        const float cc = px * px + py * py - (float)(radius * radius);
        const float disc = b * b - cc;
        if (disc < 0.0f) return false;
        const float t = std::sqrt(disc);
        const float t1 = -b - t;
        const float t2 = -b + t;
        ax = (int)lroundf(ox + ux * t1);
        ay = (int)lroundf(oy + uy * t1);
        bx = (int)lroundf(ox + ux * t2);
        by = (int)lroundf(oy + uy * t2);
        return true;
    };

    auto draw_bar = [&](float offset, uint16_t color) {
        int ax, ay, bx, by;
        if (clip_to_ball(cx + nx * offset, (float)horizon_y + ny * offset,
                         ax, ay, bx, by)) {
            fb_.drawLine(ax, ay, bx, by, color);
        }
    };

    draw_bar(-1.0f, COLOR_TEXT_DIM);
    draw_bar(0.0f, COLOR_WHITE);
    draw_bar(1.0f, COLOR_WHITE);
    draw_bar(2.0f, COLOR_TEXT_DIM);

    // Instrument bezel: dark seam, lit rim, then a two-pixel outer ring.
    if (square) {
        drawRoundedSquareOutline(cx, cy, radius, 14, COLOR_BLACK);
        drawRoundedSquareOutline(cx, cy, radius + 1, 15, COLOR_PFD_BEZEL_HI);
        drawRoundedSquareOutline(cx, cy, radius + 2, 16, COLOR_PFD_BEZEL);
        drawRoundedSquareOutline(cx, cy, radius + 3, 17, COLOR_PFD_BEZEL);
    } else {
        drawCircleApprox(cx, cy, radius, COLOR_BLACK);
        drawCircleApprox(cx, cy, radius + 1, COLOR_PFD_BEZEL_HI);
        drawCircleApprox(cx, cy, radius + 2, COLOR_PFD_BEZEL);
        drawCircleApprox(cx, cy, radius + 3, COLOR_PFD_BEZEL);
    }
}

void ADSBUIRenderer::drawPitchLadder(int cx, int cy, int radius,
                                     float current_pitch, float bank_angle) {
    const float pixels_per_degree = 2.5f;
    const float gap = 10.0f;

    float bank = bank_angle;
    if (bank > 45.0f) bank = 45.0f;
    if (bank < -45.0f) bank = -45.0f;
    const float phi = bank * (float)M_PI / 180.0f;
    const float ux = std::cos(phi);
    const float uy = std::sin(phi);
    const float nx = -uy;
    const float ny = ux;
    const float clip_r = (float)(radius - 3);

    for (int pitch = -30; pitch <= 30; pitch += 5) {
        if (pitch == 0) continue;  // horizon bar is drawn by the ball

        // Ladder line centre, offset along the screen vertical so the pitch
        // spacing stays uniform while the line itself rolls with the bank.
        const float y0 =
            (float)cy + (current_pitch - (float)pitch) * pixels_per_degree;
        const float py = y0 - (float)cy;
        const float b = py * uy;
        const float cc = py * py - clip_r * clip_r;
        const float disc = b * b - cc;
        if (disc <= 0.0f) continue;

        const float half = std::sqrt(disc);
        const float t1 = -b - half;
        const float t2 = -b + half;
        if (t2 - t1 < 2.0f * gap + 6.0f) continue;

        const bool major = (pitch % 10 == 0);
        const uint16_t color = major ? COLOR_PFD_LADDER : COLOR_PFD_TAPE_TICK;

        auto draw_rot = [&](float ta, float tb, bool dashed) {
            const int ax = (int)lroundf((float)cx + ux * ta);
            const int ay = (int)lroundf(y0 + uy * ta);
            const int bx = (int)lroundf((float)cx + ux * tb);
            const int by = (int)lroundf(y0 + uy * tb);
            if (dashed) {
                const int len = (int)lroundf(tb - ta);
                for (int i = 0; i <= len; ++i) {
                    if (((i / 2) % 2) != 0) continue;
                    fb_.drawPixel((int)lroundf((float)ax + ux * i),
                                  (int)lroundf((float)ay + uy * i), color);
                }
                return;
            }
            fb_.drawLine(ax, ay, bx, by, color);
            if (major) {
                fb_.drawLine(ax + (int)lroundf(nx), ay + (int)lroundf(ny),
                             bx + (int)lroundf(nx), by + (int)lroundf(ny),
                             color);
            }
        };

        // Minor pitch marks stop short of the ball edge so the 10-degree
        // lines stay visually dominant.
        const float shorten = major ? 0.0f : 7.0f;
        draw_rot(t1 + shorten, -gap, pitch < 0);
        draw_rot(gap, t2 - shorten, pitch < 0);

        if (major && (t2 - t1) > 52.0f) {
            // Below-horizon marks carry a minus sign, like a real PFD.
            char label[8];
            snprintf(label, sizeof(label), "%s%d",
                     (pitch < 0) ? "-" : "", std::abs(pitch));
            const int label_w = (int)strlen(label) * 7;

            const int lx = (int)lroundf((float)cx + ux * t1);
            const int ly = (int)lroundf(y0 + uy * t1) - 8;
            drawSmallText(lx + 1, ly + 1, label, COLOR_BLACK);
            drawSmallText(lx + 1, ly, label, COLOR_WHITE);

            const int rx = (int)lroundf((float)cx + ux * t2) - label_w - 2;
            const int ry = (int)lroundf(y0 + uy * t2) - 8;
            drawSmallText(rx + 1, ry + 1, label, COLOR_BLACK);
            drawSmallText(rx, ry, label, COLOR_WHITE);
        }
    }
}

void ADSBUIRenderer::drawBankAngleIndicator(int cx, int cy, int radius,
                                            float bank_angle, bool square) {
    if (square) {
        const int scale_y = cy - radius - 14;
        const int span = (radius * 4) / 5;

        for (int angle = -60; angle <= 60; angle += 10) {
            const bool major = (angle == 0 || std::abs(angle) == 30 ||
                                std::abs(angle) == 60);
            const int x = cx + angle * span / 60;
            const int len = major ? 6 : 3;
            fb_.drawLine(x, scale_y, x, scale_y + len,
                         major ? COLOR_WHITE : COLOR_PFD_TAPE_TICK);
        }

        float bank = bank_angle;
        if (bank > 60.0f) bank = 60.0f;
        if (bank < -60.0f) bank = -60.0f;
        const int px = cx + (int)lroundf(bank * span / 60.0f);
        fb_.drawLine(px, scale_y + 8, px - 2, scale_y + 11, COLOR_YELLOW);
        fb_.drawLine(px, scale_y + 8, px + 2, scale_y + 11, COLOR_YELLOW);
        fb_.drawLine(px - 2, scale_y + 11, px + 2, scale_y + 11,
                     COLOR_YELLOW);
        return;
    }

    static const int angles[] = {-60, -45, -30, -20, -10,
                                 0,   10,  20,  30,  45,  60};

    for (int angle : angles) {
        const float rad = angle * (float)M_PI / 180.0f;
        const float s = std::sin(rad);
        const float c = std::cos(rad);

        const bool major = (angle == 0 || std::abs(angle) == 30 ||
                            std::abs(angle) == 60);
        const int r0 = radius + 3;
        const int r1 = radius + (major ? 8 : 5);
        const uint16_t color = major ? COLOR_WHITE : COLOR_PFD_TAPE_TICK;

        fb_.drawLine(cx + (int)lroundf(s * r0), cy - (int)lroundf(c * r0),
                     cx + (int)lroundf(s * r1), cy - (int)lroundf(c * r1),
                     color);
    }

    // Moving bank pointer, a small yellow triangle that slides along the
    // scale as the aircraft turns.
    float bank = bank_angle;
    if (bank > 60.0f) bank = 60.0f;
    if (bank < -60.0f) bank = -60.0f;

    const float rad = bank * (float)M_PI / 180.0f;
    const float s = std::sin(rad);
    const float c = std::cos(rad);

    auto draw_pointer = [&](int tip_r, int base_r, int half_width,
                            uint16_t color) {
        const int steps = base_r - tip_r;
        for (int i = 0; i <= steps; ++i) {
            const float t = (float)i / (float)steps;
            const float rr = (float)(tip_r + i);
            const int rx = cx + (int)lroundf(s * rr);
            const int ry = cy - (int)lroundf(c * rr);
            const int hw = (int)lroundf((float)half_width * t);
            const int ox = (int)lroundf(c * hw);
            const int oy = (int)lroundf(s * hw);
            fb_.drawLine(rx - ox, ry - oy, rx + ox, ry + oy, color);
        }
    };

    draw_pointer(radius + 1, radius + 8, 4, COLOR_BLACK);
    draw_pointer(radius + 2, radius + 7, 3, COLOR_YELLOW);
}

void ADSBUIRenderer::drawAircraftSymbol(int cx, int cy) {
    // Black backing keeps the reference symbol readable over sky and ground.
    fb_.fillRect(cx - 19, cy - 1, 13, 4, COLOR_BLACK);
    fb_.fillRect(cx + 7, cy - 1, 13, 4, COLOR_BLACK);
    fb_.fillRect(cx - 2, cy - 2, 5, 5, COLOR_BLACK);

    fb_.fillRect(cx - 18, cy, 11, 2, COLOR_YELLOW);
    fb_.fillRect(cx + 8, cy, 11, 2, COLOR_YELLOW);
    fb_.fillRect(cx - 18, cy - 1, 2, 4, COLOR_YELLOW);
    fb_.fillRect(cx + 17, cy - 1, 2, 4, COLOR_YELLOW);
    fb_.fillRect(cx - 1, cy - 1, 2, 2, COLOR_YELLOW);
}

void ADSBUIRenderer::drawAltitudeTape(int x, int y, int w, int h,
                                      float altitude) {
    const int center_y = y + h / 2;
    const float px_per_ft = 0.05f;
    const int whole = (int)std::floor(altitude + 0.0001f);

    // Ticks every 500 ft, labelled every 1000 ft in a compact form.
    const int base = (whole / 500) * 500;
    for (int alt = base - 1500; alt <= base + 1500; alt += 500) {
        if (alt < 0) continue;
        const int ty =
            center_y - (int)lroundf(((float)alt - altitude) * px_per_ft);
        if (ty < y + 14 || ty > y + h - 4) continue;

        const bool major = (alt % 1000 == 0);
        fb_.drawLine(x, ty, x + (major ? 8 : 5), ty, COLOR_PFD_TAPE_TICK);

        if (major) {
            char label[8];
            if (alt >= 10000) {
                snprintf(label, sizeof(label), "%dk", alt / 1000);
            } else {
                snprintf(label, sizeof(label), "%d", alt);
            }
            drawSmallText(x + 10, ty - 3, label, COLOR_PFD_TAPE_TICK);
        }
    }

    // Frameless readout window: a little taller than one digit so the
    // adjacent numbers on the wheels stay visible above and below.
    const int box_y = center_y - 11;
    const int ty = box_y + 3;
    const int tx = x + 1;
    const int clip_top = ty;
    const int clip_bottom = ty + 15;
    fb_.fillRect(x, box_y, w, 22, COLOR_PFD_PANEL);

    // Leading three digits are static; they are not wheels.
    int base_val = whole;
    if (altitude - (float)whole >= 0.5f) base_val = whole + 1;

    char lead_str[8];
    snprintf(lead_str, sizeof(lead_str), "%03d", (base_val / 100) % 1000);
    for (int i = 0; i < 3; ++i) {
        drawSegDigit(tx + i * 7, ty, lead_str[i] - '0', ty, ty + 15);
    }

    // Last two digits are independent wheels. The ones wheel rolls with the
    // fractional foot; the tens wheel only turns during the 9->0 carry.
    const int ones = whole % 10;
    const float frac = altitude - (float)whole;
    const int tens = (whole / 10) % 10;
    const float tens_phase = (ones == 9) ? frac : 0.0f;

    auto draw_wheel = [&](int wx, int digit, float phase) {
        const int shift = (int)lroundf(phase * 16.0f);
        drawSegDigit(wx, ty - shift, digit, clip_top, clip_bottom);
        drawSegDigit(wx, ty + 16 - shift, (digit + 1) % 10, clip_top,
                     clip_bottom, COLOR_PFD_SEG_DIM);
        drawSegDigit(wx, ty - 16 - shift, (digit + 9) % 10, clip_top,
                     clip_bottom, COLOR_PFD_SEG_DIM);
    };

    draw_wheel(tx + 21, tens, tens_phase);
    draw_wheel(tx + 28, ones, frac);
}

void ADSBUIRenderer::drawSpeedTape(int x, int y, int w, int h, int speed) {
    const int center_y = y + h / 2;
    const float px_per_kt = 0.5f;

    const int base = (speed / 50) * 50;
    for (int s = base - 200; s <= base + 200; s += 50) {
        if (s < 0) continue;
        const int ty = center_y - (int)lroundf((s - speed) * px_per_kt);
        if (ty < y + 14 || ty > y + h - 4) continue;

        const bool major = (s % 100 == 0);
        fb_.drawLine(x + w - 1, ty, x + w - (major ? 8 : 5), ty,
                     COLOR_PFD_TAPE_TICK);

        if (major) {
            char label[8];
            snprintf(label, sizeof(label), "%d", s);
            const int label_w = (int)strlen(label) * 7;
            drawSmallText(x + w - 10 - label_w, ty - 3, label,
                          COLOR_PFD_TAPE_TICK);
        }
    }

    const int box_y = center_y - 11;
    // Mask the scale behind the readout band, but start one digit in so the
    // black backing never covers the metal frame on the left.
    fb_.fillRect(x + 8, box_y, w - 8, 22, COLOR_PFD_PANEL);
    char spd_str[8];
    snprintf(spd_str, sizeof(spd_str), "%d", speed);
    // The readout window is one digit wider than the value, so the extra
    // dark area covers the scale labels on the left without touching the
    // metal frame.
    const int digits = (int)strlen(spd_str);
    int box_w = std::max(23, digits * 7 + 2) + 7;
    if (box_w > w) box_w = w;

    // Widen the mask by another digit and a few pixels vertically so the
    // scale labels cannot peek around the readout, while staying inside the
    // face (the metal frame ends at x + 3).
    fb_.fillRect(x + 4, box_y - 7, w - 4, 30, COLOR_PFD_PANEL);
    drawReadout(x + w - box_w, box_y, box_w, 22, spd_str, true);
}

void ADSBUIRenderer::drawHeadingIndicator(int x, int y, int w, int h,
                                          int heading) {
    const int cx = x + w / 2;
    const int visible_range = 120;
    const float px_per_deg = (float)w / (float)visible_range;

    auto hdg_x = [&](int hdg) {
        int diff = ((hdg - heading) % 360 + 540) % 360 - 180;
        return cx + (int)lroundf(diff * px_per_deg);
    };

    // Ticks every 5 degrees, longer every 10 and 30.
    for (int hdg = 0; hdg < 360; hdg += 5) {
        const int tx = hdg_x(hdg);
        if (tx < x + 3 || tx > x + w - 3) continue;
        if (hdg % 30 == 0) {
            fb_.drawLine(tx, y + h - 8, tx, y + h - 3, COLOR_PFD_TAPE_TICK);
        } else if (hdg % 10 == 0) {
            fb_.drawLine(tx, y + h - 7, tx, y + h - 3, COLOR_PFD_TAPE_TICK);
        } else {
            fb_.drawLine(tx, y + h - 5, tx, y + h - 3, COLOR_PFD_TAPE_TICK);
        }
    }

    // Labels every 30 degrees; cardinals keep their letters.
    for (int hdg = 0; hdg < 360; hdg += 30) {
        const int tx = hdg_x(hdg);
        if (tx < x + 10 || tx > x + w - 10) continue;

        char buf[8];
        const char* label = buf;
        switch (hdg) {
            case 0:   label = "N"; break;
            case 90:  label = "E"; break;
            case 180: label = "S"; break;
            case 270: label = "W"; break;
            default:
                snprintf(buf, sizeof(buf), "%02d", hdg / 10);
                break;
        }

        const int label_w = (int)strlen(label) * 7;
        drawSmallText(tx - label_w / 2, y + 4, label, COLOR_PFD_LADDER);
    }

    // Fixed lubber line: a magenta triangle below the tick scale, pointing
    // up at the current heading at the centre of the tape.
    fb_.drawLine(cx, y + h, cx - 3, y + h + 4, COLOR_MAGENTA);
    fb_.drawLine(cx, y + h, cx + 3, y + h + 4, COLOR_MAGENTA);
    fb_.drawLine(cx - 3, y + h + 4, cx + 3, y + h + 4, COLOR_MAGENTA);
}

void ADSBUIRenderer::renderPFD(const AircraftState& aircraft, int x, int y,
                               int w, int h, bool square) {
    fb_.fillRect(x, y, w, h, COLOR_PFD_PANEL);

    const int CX = x + w / 2;
    const int CENTER_Y = y + 80;
    const int RADIUS = 58;
    const int TAPE_W = 37;
    const int TAPE_H = 136;
    const int TAPE_Y = CENTER_Y - TAPE_H / 2;
    const int speed_x = x + 1;
    const int alt_x = x + w - TAPE_W - 1;

    if (aircraft.velocity_valid && aircraft.speed > 0) {
        drawSpeedTape(speed_x, TAPE_Y, TAPE_W, TAPE_H, aircraft.speed);
    }
    if (aircraft.position_valid && aircraft.altitude > 0) {
        const float alt_display = (aircraft.altitude_display > 0.5f)
                                      ? aircraft.altitude_display
                                      : (float)aircraft.altitude;
        drawAltitudeTape(alt_x, TAPE_Y, TAPE_W, TAPE_H, alt_display);
    }

    if (aircraft.velocity_valid || aircraft.position_valid) {
        const float pitch_angle = calculatePitchAngle(aircraft);
        drawArtificialHorizon(CX, CENTER_Y, RADIUS, pitch_angle,
                              aircraft.bank, square);
        drawPitchLadder(CX, CENTER_Y, RADIUS, pitch_angle, aircraft.bank);
        drawBankAngleIndicator(CX, CENTER_Y, RADIUS, aircraft.bank, square);
        drawAircraftSymbol(CX, CENTER_Y);
    }

    if (aircraft.velocity_valid && aircraft.heading >= 0) {
        const int hdg_w = 140;
        const int hdg_y = y + h - 28;
        const int hdg_h = 20;
        drawHeadingIndicator(CX - hdg_w / 2, hdg_y, hdg_w, hdg_h,
                             aircraft.heading);
    }
}

void ADSBUIRenderer::renderText(int x, int y, const char* text, uint16_t color, bool bold) {
    // Simply delegate to drawText (bold parameter ignored)
    drawText(x, y, text, color);
}

void ADSBUIRenderer::drawRoundedPanel(int x, int y, int w, int h, int radius,
                                      uint16_t color, float alpha) {
    if (w <= 0 || h <= 0) return;
    radius = std::max(0, std::min(radius, std::min(w / 2, h / 2)));
    for (int row = 0; row < h; ++row) {
        int inset = 0;
        if (row < radius) {
            const float dy = radius - (row + 0.5f);
            inset = radius - static_cast<int>(std::lround(
                                 std::sqrt(radius * radius - dy * dy)));
        } else if (row >= h - radius) {
            const float dy = (row + 0.5f) - (h - radius);
            inset = radius - static_cast<int>(std::lround(
                                 std::sqrt(radius * radius - dy * dy)));
        }
        for (int col = 0; col < w - 2 * inset; ++col) {
            fb_.blendPixel(x + inset + col, y + row, color, alpha);
        }
    }
}

void ADSBUIRenderer::renderKeyHelp() {
    // Dim the page behind the translucent help panel.
    for (int y = 0; y < 170; ++y) {
        for (int x = 0; x < 320; ++x) {
            fb_.blendPixel(x, y, COLOR_BLACK, 0.45f);
        }
    }

    const int panel_w = 252;
    const int panel_h = 152;
    const int radius = 9;
    const int px = (320 - panel_w) / 2;
    const int py = (170 - panel_h) / 2;
    drawRoundedPanel(px, py, panel_w, panel_h, radius, COLOR_DIALOG_BG, 0.85f);

    drawSmallText(px + 12, py + 9, "KEYS", COLOR_YELLOW);
    drawSmallText(px + 60, py + 9, "H close   <> settings",
                  COLOR_TEXT_DIM);

    struct Row { const char* key; const char* action; };
    static const Row rows[] = {
        {"TAB",        "PAGE MENU"},
        {"UP/DOWN",    "SELECT AIRCRAFT"},
        {"HOLD F",     "JUMP TO TOP"},
        {"LEFT/RIGHT", "PAGE 5 ROWS"},
        {"ENTER",      "RADAR>DETAIL>LIST"},
        {"I / 0",      "COMPASS UP / NORTH"},
        {"L",          "SETTINGS / MAP"},
        {"H",          "HELP / SETTINGS"},
        {"G / +/-",    "GAIN (LIST)"},
        {"M",          "PFD / RADAR THEME"},
        {"R / T",      "ZOOM (RADAR)"},
        {"FN+D/S",     "VOLUME"},
        {"HOLD ESC",   "QUIT"},
    };

    const int count = (int)(sizeof(rows) / sizeof(rows[0]));
    const int step = count > 11 ? 10 : 11;
    int y = py + 24;
    for (int i = 0; i < count; ++i) {
        const auto& r = rows[i];
        drawSmallText(px + 14, y, r.key, COLOR_YELLOW);
        drawSmallText(px + 92, y, r.action, COLOR_WHITE);
        y += step;
    }
}

void ADSBUIRenderer::renderSettingsOverview(double lat, double lon,
                                            bool pos_set, int selected,
                                            const char* source,
                                            const char* map_provider) {
    for (int y = 0; y < 170; ++y) {
        for (int x = 0; x < 320; ++x) {
            fb_.blendPixel(x, y, COLOR_BLACK, 0.45f);
        }
    }

    const int panel_w = 260;
    const int panel_h = 146;
    const int radius = 9;
    const int px = (320 - panel_w) / 2;
    const int py = (170 - panel_h) / 2;
    drawRoundedPanel(px, py, panel_w, panel_h, radius, COLOR_DIALOG_BG, 0.85f);

    drawSmallText(px + 12, py + 9, "SETTINGS", COLOR_YELLOW);
    drawSmallText(px + 80, py + 9, "H close   <> keys", COLOR_TEXT_DIM);

    char lat_s[32];
    char lon_s[32];
    if (pos_set) {
        snprintf(lat_s, sizeof(lat_s), "%.4f", lat);
        snprintf(lon_s, sizeof(lon_s), "%.4f", lon);
    } else {
        snprintf(lat_s, sizeof(lat_s), "NOT SET");
        snprintf(lon_s, sizeof(lon_s), "NOT SET");
    }

    static const char* labels[4] = {"LATITUDE", "LONGITUDE", "MAP SOURCE",
                                    "CLEAR POSITION"};
    const char* values[4] = {lat_s, lon_s,
                             map_provider ? map_provider : "-",
                             pos_set ? "ENTER to clear" : "-"};

    for (int i = 0; i < 4; ++i) {
        const int y = py + 36 + i * 24;
        if (i == selected) {
            fb_.fillRect(px + 6, y - 6, panel_w - 12, 16, COLOR_SELECTED);
            drawSmallText(px + 10, y, ">", COLOR_YELLOW);
        }
        drawSmallText(px + 18, y, labels[i],
                      i == selected ? COLOR_WHITE : COLOR_TEXT_BRIGHT);
        drawSmallText(px + 150, y, values[i], COLOR_YELLOW);
    }

    char src_line[32];
    snprintf(src_line, sizeof(src_line), "SOURCE: %s", source ? source : "-");
    drawSmallText(px + 18, py + 126, src_line, COLOR_TEXT_DIM);
    drawSmallText(px + 18, py + 138, "UP/DOWN SELECT  ENTER  H CLOSE",
                  COLOR_TEXT_DIM);
}
