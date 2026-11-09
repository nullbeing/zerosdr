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

void ADSBUIRenderer::renderStatusBar(int aircraft_count,
                                     uint32_t message_count,
                                     float message_rate) {
    // Clear status bar area with blue background
    fb_.fillRect(0, STATUS_BAR_Y, 320, STATUS_BAR_H, COLOR_ADSB_STATUSBAR_BG);

    // Format status string: "AC:5 Msg:1234 45/s 1090MHz"
    char status[64];
    snprintf(status, sizeof(status), "AC:%d Msg:%u %.0f/s 1090MHz",
             aircraft_count, message_count, message_rate);

    drawText(2, STATUS_BAR_Y + 2, status, COLOR_ADSB_STATUSBAR_TEXT);
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

    drawText(2, STATS_BAR_Y + 2, stats, COLOR_ADSB_STATUSBAR_TEXT);
}

void ADSBUIRenderer::renderAircraftDetail(const AircraftState& aircraft,
                                          int scroll_offset,
                                          int aircraft_count,
                                          uint32_t message_count,
                                          float message_rate,
                                          float gain) {
    // Clear screen
    fb_.clear(COLOR_BG_DARK);

    // Define layout areas - give more space to PFD
    const int PFD_X = 0;
    const int PFD_Y = 0;
    const int PFD_W = 180;  // Increased from 140 to 180
    const int PFD_H = STATS_BAR_Y;  // 152px

    const int INFO_X = 180;  // Moved from 140 to 180
    const int INFO_Y = 0;
    const int INFO_W = 140;  // Reduced from 180 to 140
    const int INFO_H = STATS_BAR_Y;  // 152px

    // Render PFD on left side
    renderPFD(aircraft, PFD_X, PFD_Y, PFD_W, PFD_H);

    // Build detail text lines
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

    // Clear info area
    fb_.fillRect(INFO_X, INFO_Y, INFO_W, INFO_H, COLOR_BG_DARK);

    // Render scrollable text on right side
    const int LINE_HEIGHT = 18;
    int y = INFO_Y + 5 - scroll_offset;

    for (size_t i = 0; i < lines.size(); i++) {
        if (y + LINE_HEIGHT > INFO_Y && y < STATS_BAR_Y) {
            drawText(INFO_X + 5, y, lines[i].c_str(), COLOR_TEXT_BRIGHT);
        }
        y += LINE_HEIGHT;
    }

    // Render bottom status bar
    fb_.fillRect(0, STATS_BAR_Y, 320, STATS_BAR_H, COLOR_ADSB_STATUSBAR_BG);
    drawText(2, STATS_BAR_Y + 2, "Enter:Radar  Up/Down:Scroll", COLOR_ADSB_STATUSBAR_TEXT);
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

void ADSBUIRenderer::drawArtificialHorizon(int cx, int cy, int radius, float pitch_angle) {
    const float pixels_per_degree = 2.5f;

    // Calculate vertical offset for horizon line
    // Positive pitch (nose up) = horizon moves down on display
    int y_offset = (int)(pitch_angle * pixels_per_degree);

    // Clamp offset to keep horizon within visible circle
    int max_offset = radius - 5;
    if (y_offset > max_offset) y_offset = max_offset;
    if (y_offset < -max_offset) y_offset = -max_offset;

    int horizon_y = cy + y_offset;

    // Define clipping width (two-sided cut)
    int clip_width = radius * 1.6;
    int clip_left = cx - clip_width/2;
    int clip_right = cx + clip_width/2;

    // Fill sky (above horizon line) with bright sky blue
    for (int dy = -radius; dy <= radius; dy++) {
        int current_y = cy + dy;

        // Skip if below horizon
        if (current_y >= horizon_y) continue;

        int dx = (int)sqrt(radius * radius - dy * dy);
        int x_start = std::max(clip_left, cx - dx);
        int x_end = std::min(clip_right, cx + dx);
        if (x_start < x_end) {
            fb_.drawLine(x_start, current_y, x_end, current_y, COLOR_SKY_BLUE);
        }
    }

    // Fill ground (below horizon line) with earth brown
    for (int dy = -radius; dy <= radius; dy++) {
        int current_y = cy + dy;

        // Skip if above horizon
        if (current_y < horizon_y) continue;

        int dx = (int)sqrt(radius * radius - dy * dy);
        int x_start = std::max(clip_left, cx - dx);
        int x_end = std::min(clip_right, cx + dx);
        if (x_start < x_end) {
            fb_.drawLine(x_start, current_y, x_end, current_y, COLOR_EARTH_BROWN);
        }
    }

    // Draw horizon line at offset position (clipped, white)
    fb_.drawLine(clip_left, horizon_y, clip_right, horizon_y, COLOR_WHITE);

    // Draw aircraft symbol (yellow cross) - always at center (fixed reference)
    fb_.drawLine(cx - 15, cy, cx - 5, cy, COLOR_YELLOW);
    fb_.drawLine(cx + 5, cy, cx + 15, cy, COLOR_YELLOW);
    fb_.drawLine(cx, cy - 5, cx, cy + 5, COLOR_YELLOW);
}

void ADSBUIRenderer::drawPitchLadder(int cx, int cy, int width, int height, float current_pitch) {
    const float pixels_per_degree = 2.5f;

    // Calculate offset for current pitch
    // This makes the pitch ladder move with the horizon
    int pitch_offset = (int)(current_pitch * pixels_per_degree);

    // Draw pitch marks every 5 degrees from -30 to +30
    for (int pitch = -30; pitch <= 30; pitch += 5) {
        if (pitch == 0) continue;  // Skip horizon line (drawn separately)

        // Calculate mark position relative to current pitch
        int y_offset = -pitch * pixels_per_degree + pitch_offset;
        int mark_y = cy + y_offset;

        // Skip if outside visible area
        if (mark_y < cy - height/2 || mark_y > cy + height/2) continue;

        // Longer marks for 10-degree intervals
        int mark_width = (pitch % 10 == 0) ? 14 : 10;

        // Draw left and right pitch marks (green)
        fb_.drawLine(cx - width/2, mark_y, cx - width/2 + mark_width, mark_y, COLOR_GREEN);
        fb_.drawLine(cx + width/2 - mark_width, mark_y, cx + width/2, mark_y, COLOR_GREEN);

            // Draw text labels only for 10-degree intervals
            if (pitch % 10 == 0) {
                char label[8];
                snprintf(label, sizeof(label), "%d", abs(pitch));

                // Left label - pushed outward toward speed tape
                drawSmallText(cx - width/2 - 22, mark_y - 3, label, COLOR_WHITE);

                // Right label - pushed outward toward altitude tape
                drawSmallText(cx + width/2 + 8, mark_y - 3, label, COLOR_WHITE);
            }
    }
}

void ADSBUIRenderer::drawBankAngleIndicator(int cx, int cy, int radius) {
    // Draw bank angle scale arc at bottom
    const int bank_angles[] = {-60, -45, -30, -20, -10, 0, 10, 20, 30, 45, 60};

    for (int angle : bank_angles) {
        // Convert angle to radians
        float rad = angle * M_PI / 180.0f;

        // Calculate mark position on bottom arc
        int mark_x = cx + (int)(radius * sin(rad));
        int mark_y = cy + (int)(radius * cos(rad));

        // Draw tick mark (longer for major angles)
        int tick_length = (angle % 30 == 0) ? 5 : 3;
        int tick_end_x = cx + (int)((radius + tick_length) * sin(rad));
        int tick_end_y = cy + (int)((radius + tick_length) * cos(rad));

        fb_.drawLine(mark_x, mark_y, tick_end_x, tick_end_y, COLOR_GREEN);

        // Draw text labels for major angles (30, 60)
        // Center text on the tick mark for left/right symmetry
        if (angle % 30 == 0 && angle != 0) {
            char label[8];
            snprintf(label, sizeof(label), "%d", abs(angle));
            int text_x = mark_x - 7;  // 14px text width / 2
            int text_y = mark_y - 4;  // 8px font height / 2
            drawSmallText(text_x, text_y, label, COLOR_WHITE);
        }
    }

    // Draw center triangle pointer at top (0°) - this is the fixed reference
    fb_.drawLine(cx, cy - radius - 4, cx - 2, cy - radius, COLOR_WHITE);
    fb_.drawLine(cx, cy - radius - 4, cx + 2, cy - radius, COLOR_WHITE);
    fb_.drawLine(cx - 2, cy - radius, cx + 2, cy - radius, COLOR_WHITE);
}

void ADSBUIRenderer::drawAltitudeTape(int x, int y, int w, int h, int altitude) {
    // Draw tape background
    fb_.fillRect(x, y, w, h, 0x2104);  // Dark background
    fb_.drawRect(x, y, w, h, COLOR_GRID);
    
    // Draw altitude value in center
    char alt_str[8];
    snprintf(alt_str, sizeof(alt_str), "%d", altitude);
    int text_y = y + h/2 - 8;
    fb_.fillRect(x + 1, text_y, w - 2, 16, COLOR_BLACK);
    drawText(x + 2, text_y, alt_str, COLOR_GREEN);
    
    // Draw tick marks (every 1000 ft)
    int alt_base = (altitude / 1000) * 1000;
    for (int i = -2; i <= 2; i++) {
        int tick_alt = alt_base + i * 1000;
        int tick_y = y + h/2 - ((altitude - tick_alt) * h / 5000);
        if (tick_y >= y && tick_y <= y + h) {
            fb_.drawLine(x, tick_y, x + 5, tick_y, COLOR_GRID);
        }
    }
}

void ADSBUIRenderer::drawSpeedTape(int x, int y, int w, int h, int speed) {
    // Draw tape background
    fb_.fillRect(x, y, w, h, 0x2104);
    fb_.drawRect(x, y, w, h, COLOR_GRID);
    
    // Draw speed value in center
    char spd_str[8];
    snprintf(spd_str, sizeof(spd_str), "%d", speed);
    int text_y = y + h/2 - 8;
    fb_.fillRect(x + 1, text_y, w - 2, 16, COLOR_BLACK);
    drawText(x + 2, text_y, spd_str, COLOR_GREEN);
    
    // Draw tick marks (every 50 kt)
    int spd_base = (speed / 50) * 50;
    for (int i = -2; i <= 2; i++) {
        int tick_spd = spd_base + i * 50;
        if (tick_spd < 0) continue;
        int tick_y = y + h/2 - ((speed - tick_spd) * h / 250);
        if (tick_y >= y && tick_y <= y + h) {
            fb_.drawLine(x + w - 5, tick_y, x + w, tick_y, COLOR_GRID);
        }
    }
}

// Helper function to get direction label for a given heading
static const char* getDirectionLabel(int heading) {
    // Normalize heading to 0-359 range
    heading = ((heading % 360) + 360) % 360;

    if (heading >= 337 || heading < 23) return "N";
    if (heading >= 23 && heading < 68) return "NE";
    if (heading >= 68 && heading < 113) return "E";
    if (heading >= 113 && heading < 158) return "SE";
    if (heading >= 158 && heading < 203) return "S";
    if (heading >= 203 && heading < 248) return "SW";
    if (heading >= 248 && heading < 293) return "W";
    if (heading >= 293 && heading < 337) return "NW";
    return "N";
}

void ADSBUIRenderer::drawHeadingIndicator(int x, int y, int w, int h, int heading) {
    // Draw tape background
    fb_.fillRect(x, y, w, h, 0x2104);  // Dark background
    fb_.drawRect(x, y, w, h, COLOR_GRID);

    // Calculate center position
    int cx = x + w / 2;
    int cy = y + h / 2;

    // Visible range: ±60° from current heading (120° total)
    const int visible_range = 120;
    const float pixels_per_degree = (float)w / (float)visible_range;

    // Draw major direction labels (every 45°)
    for (int hdg = 0; hdg < 360; hdg += 45) {
        // Calculate angular difference (handle wraparound)
        int diff = hdg - heading;
        if (diff > 180) diff -= 360;
        if (diff < -180) diff += 360;

        // Skip if outside visible range
        if (diff < -60 || diff > 60) continue;

        // Calculate x position
        int tick_x = cx + (int)(diff * pixels_per_degree);

        // Draw direction label in middle area
        const char* dir_label = "";
        if (hdg == 0 || hdg == 360) dir_label = "N";
        else if (hdg == 45) dir_label = "NE";
        else if (hdg == 90) dir_label = "E";
        else if (hdg == 135) dir_label = "SE";
        else if (hdg == 180) dir_label = "S";
        else if (hdg == 225) dir_label = "SW";
        else if (hdg == 270) dir_label = "W";
        else if (hdg == 315) dir_label = "NW";

        int label_w = strlen(dir_label) * (FONT_6X8_WIDTH + 1);
        drawSmallText(tick_x - label_w/2, cy - 3, dir_label, COLOR_WHITE);

        // Draw tick mark at bottom
        fb_.drawLine(tick_x, y + h - 6, tick_x, y + h - 1, COLOR_GRID);
    }

    // Draw minor tick marks (every 15°) without labels
    for (int hdg = 0; hdg < 360; hdg += 15) {
        // Skip major directions (already drawn)
        if (hdg % 45 == 0) continue;

        // Calculate angular difference (handle wraparound)
        int diff = hdg - heading;
        if (diff > 180) diff -= 360;
        if (diff < -180) diff += 360;

        // Skip if outside visible range
        if (diff < -60 || diff > 60) continue;

        // Calculate x position
        int tick_x = cx + (int)(diff * pixels_per_degree);

        // Draw shorter tick mark at bottom
        fb_.drawLine(tick_x, y + h - 3, tick_x, y + h - 1, COLOR_GRID);
    }

    // Draw fixed triangle pointer at top center (aircraft reference)
    fb_.drawLine(cx, y + 1, cx - 3, y + 5, COLOR_MAGENTA);
    fb_.drawLine(cx, y + 1, cx + 3, y + 5, COLOR_MAGENTA);
    fb_.drawLine(cx - 3, y + 5, cx + 3, y + 5, COLOR_MAGENTA);
}

void ADSBUIRenderer::drawVerticalSpeedIndicator(int x, int y, int w, int h, int vs) {
    // No border - removed for cleaner look

    // Calculate center position (where altitude value is displayed)
    int cy = y + h/2;

    if (vs > 100) {
        // Climbing - green arrow up (above altitude value)
        int arrow_y = cy - 12;  // Position above altitude text
        // Draw upward arrow
        fb_.drawLine(x, arrow_y, x, arrow_y - 6, COLOR_GREEN);  // Vertical line
        fb_.drawLine(x, arrow_y - 6, x - 2, arrow_y - 3, COLOR_GREEN);  // Left wing
        fb_.drawLine(x, arrow_y - 6, x + 2, arrow_y - 3, COLOR_GREEN);  // Right wing
    } else if (vs < -100) {
        // Descending - red arrow down (below altitude value)
        int arrow_y = cy + 12;  // Position below altitude text
        // Draw downward arrow
        fb_.drawLine(x, arrow_y, x, arrow_y + 6, COLOR_RED);  // Vertical line
        fb_.drawLine(x, arrow_y + 6, x - 2, arrow_y + 3, COLOR_RED);  // Left wing
        fb_.drawLine(x, arrow_y + 6, x + 2, arrow_y + 3, COLOR_RED);  // Right wing
    }
}

void ADSBUIRenderer::renderPFD(const AircraftState& aircraft, int x, int y, int w, int h) {
    // Clear PFD area
    fb_.fillRect(x, y, w, h, COLOR_BG_DARK);

    // No border - removed for cleaner look

    // Layout within PFD area
    const int MARGIN = 2;
    const int TAPE_WIDTH = 25;

    // Artificial horizon in center
    int horizon_cx = x + w/2;
    int horizon_cy = y + h/2;
    int horizon_radius = 45;  // Increased from 40 to 45 for more space

    // Calculate pitch angle from aircraft data
    float pitch_angle = calculatePitchAngle(aircraft);

    if (aircraft.velocity_valid || aircraft.position_valid) {
        drawArtificialHorizon(horizon_cx, horizon_cy, horizon_radius, pitch_angle);
        drawPitchLadder(horizon_cx, horizon_cy, horizon_radius * 1.8, horizon_radius * 2, pitch_angle);
        drawBankAngleIndicator(horizon_cx, horizon_cy, horizon_radius);
    }
    
    // Speed tape on left
    if (aircraft.velocity_valid && aircraft.speed > 0) {
        drawSpeedTape(x + MARGIN, y + 20, TAPE_WIDTH, h - 60, aircraft.speed);
    }
    
    // Altitude tape on right
    if (aircraft.position_valid && aircraft.altitude > 0) {
        drawAltitudeTape(x + w - TAPE_WIDTH - MARGIN, y + 20, TAPE_WIDTH, h - 60, aircraft.altitude);
    }
    
    // Heading tape at bottom
    if (aircraft.velocity_valid && aircraft.heading >= 0) {
        int hdg_y = y + h - 25;
        int hdg_w = 140;  // Tape width
        int hdg_h = 22;   // Tape height
        drawHeadingIndicator(horizon_cx - hdg_w/2, hdg_y, hdg_w, hdg_h, aircraft.heading);
    }
    
    // Vertical speed indicator (small, right of altitude tape)
    if (aircraft.velocity_valid) {
        int vs_x = x + w - 10;
        int vs_y = y + 20;
        int vs_h = h - 60;
        drawVerticalSpeedIndicator(vs_x, vs_y, 8, vs_h, aircraft.vertical_rate);
    }
    
    // Callsign at top
    if (aircraft.callsign[0] != '\0') {
        int text_w = strlen(aircraft.callsign) * 9;  // 8px char + 1px spacing
        drawText(horizon_cx - text_w/2, y + 2, aircraft.callsign, COLOR_TEXT_BRIGHT);
    }
}

void ADSBUIRenderer::renderText(int x, int y, const char* text, uint16_t color, bool bold) {
    // Simply delegate to drawText (bold parameter ignored)
    drawText(x, y, text, color);
}

void ADSBUIRenderer::renderKeyHelp() {
    fb_.fillRect(0, 0, 320, 170, COLOR_BG_DARK);
    renderText(6, 4, "KEYS", COLOR_TEXT_BRIGHT, true);
    drawSmallText(60, 10, "H close   <> settings", COLOR_TEXT_DIM);

    struct Row { const char* key; const char* action; };
    static const Row rows[] = {
        {"TAB",        "SDR / ADSB"},
        {"UP/DOWN",    "SELECT AIRCRAFT"},
        {"LEFT/RIGHT", "PAGE 5 ROWS"},
        {"ENTER",      "LIST>DETAIL>RADAR"},
        {"R",          "RADAR VIEW"},
        {"L",          "POSITION SETTINGS"},
        {"H",          "HELP / SETTINGS"},
        {"G",          "GAIN MODE (RADAR)"},
        {"+ / -",      "GAIN (RADAR)"},
        {"Q / ESC",    "QUIT"},
    };

    int y = 30;
    for (const auto& r : rows) {
        drawSmallText(6, y, r.key, COLOR_YELLOW);
        drawSmallText(96, y, r.action, COLOR_WHITE);
        y += 13;
    }
}

void ADSBUIRenderer::renderSettingsOverview(double lat, double lon,
                                            bool pos_set, int selected) {
    fb_.fillRect(0, 0, 320, 170, COLOR_BG_DARK);
    renderText(6, 4, "SETTINGS", COLOR_TEXT_BRIGHT, true);
    drawSmallText(86, 10, "H close   <> keys", COLOR_TEXT_DIM);

    char lat_s[32];
    char lon_s[32];
    if (pos_set) {
        snprintf(lat_s, sizeof(lat_s), "%.4f", lat);
        snprintf(lon_s, sizeof(lon_s), "%.4f", lon);
    } else {
        snprintf(lat_s, sizeof(lat_s), "NOT SET");
        snprintf(lon_s, sizeof(lon_s), "NOT SET");
    }

    static const char* labels[3] = {"LATITUDE", "LONGITUDE", "CLEAR POSITION"};
    const char* values[3] = {lat_s, lon_s, pos_set ? "ENTER to clear" : "-"};

    for (int i = 0; i < 3; ++i) {
        const int y = 44 + i * 30;
        if (i == selected) {
            fb_.fillRect(4, y - 6, 312, 22, COLOR_SELECTED);
            drawSmallText(10, y + 2, ">", COLOR_YELLOW);
        }
        renderText(24, y, labels[i],
                   i == selected ? COLOR_WHITE : COLOR_TEXT_BRIGHT, false);
        drawSmallText(186, y + 2, values[i], COLOR_YELLOW);
    }

    drawSmallText(6, 150, "UP/DOWN select   ENTER edit/clear   H close",
                  COLOR_TEXT_DIM);
}
