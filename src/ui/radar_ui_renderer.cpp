#include "ui/radar_ui_renderer.h"
#include "font.h"
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <algorithm>

RadarUIRenderer::RadarUIRenderer(Framebuffer& fb) : BaseUIRenderer(fb) {
}

RadarUIRenderer::~RadarUIRenderer() {
}

void RadarUIRenderer::renderRadarDisplay(const std::vector<AircraftState>& aircraft,
                                         float sweep_angle,
                                         float max_range,
                                         double observer_lat,
                                         double observer_lon,
                                         int selected_index,
                                         int scroll_offset,
                                         bool blink_state) {
    // Clear entire display area with dark background
    fb_.fillRect(0, RADAR_Y, 320, RADAR_H, COLOR_RADAR_BG);

    // Draw vertical separator line between list and radar
    for (int y = RADAR_Y; y < RADAR_Y + RADAR_H; y++) {
        fb_.drawPixel(LIST_W, y, COLOR_RADAR_GRID);
    }

    // Render aircraft list in left panel
    renderAircraftList(aircraft, selected_index, scroll_offset);

    // Render radar display in right panel
    // Draw range rings
    drawRangeRings(RADAR_CENTER_X, RADAR_CENTER_Y, RADAR_RADIUS, NUM_RANGE_RINGS, max_range);

    // Translucent sweep with an angular gradient tail.
    drawSweep(RADAR_CENTER_X, RADAR_CENTER_Y, RADAR_RADIUS, sweep_angle);

    // Labels go on top of the sweep so they stay readable.
    drawCompassMarkings(RADAR_CENTER_X, RADAR_CENTER_Y, RADAR_RADIUS);
    drawRingLabels(RADAR_CENTER_X, RADAR_CENTER_Y, RADAR_RADIUS,
                   NUM_RANGE_RINGS, max_range);

    // Draw aircraft blips
    for (size_t i = 0; i < aircraft.size(); i++) {
        const auto& ac = aircraft[i];
        if (ac.position_valid && ac.distance > 0) {
            bool is_selected = (i == (size_t)selected_index);

            // If selected and blink_state is false, skip drawing (creates blink effect)
            if (is_selected && !blink_state) {
                continue;
            }
            drawAircraftBlip(RADAR_CENTER_X, RADAR_CENTER_Y,
                           ac.distance, ac.bearing,
                           ac.heading, ac.velocity_valid,
                           max_range, is_selected);
        }
    }

    // Selected callsign, 2x size, at the bottom of the left panel.
    char callsign[9] = {0};
    if (selected_index >= 0 && selected_index < (int)aircraft.size()) {
        const auto& ac = aircraft[selected_index];
        snprintf(callsign, sizeof(callsign), "%s", ac.callsign);
        for (int k = (int)strlen(callsign) - 1; k >= 0 && callsign[k] == ' '; --k) {
            callsign[k] = '\0';
        }
        if (callsign[0] == '\0') {
            snprintf(callsign, sizeof(callsign), "%06X", ac.icao_addr);
        }
    } else {
        snprintf(callsign, sizeof(callsign), "-----");
    }

    fb_.fillRect(0, CALLSIGN_BOX_Y - 2, LIST_W, 170 - CALLSIGN_BOX_Y + 2,
                 COLOR_BG_DARK);
    for (int x = 0; x < LIST_W; ++x) {
        fb_.drawPixel(x, CALLSIGN_BOX_Y - 2, COLOR_RADAR_GRID);
    }
    {
        const int scale = 2;
        int width = (int)strlen(callsign) * 7 * scale;
        int x = (LIST_W - width) / 2;
        if (x < 2) x = 2;
        drawScaledText(x, CALLSIGN_BOX_Y + 4, callsign, COLOR_RADAR_TEXT, scale);
    }
}

void RadarUIRenderer::renderStatusBar(int aircraft_count, float max_range) {
    // Clear status bar with dark background
    fb_.fillRect(0, STATUS_BAR_Y, 320, STATUS_BAR_H, COLOR_BG_DARK);

    // Format status "Aircraft: 5  Range: 50km"
    char status[64];
    snprintf(status, sizeof(status), "Aircraft:%d Range:%.0fkm",
             aircraft_count, max_range);

    // Draw status text (left-aligned)
    drawText(5, STATUS_BAR_Y + 2, status, COLOR_RADAR_TEXT);
}

void RadarUIRenderer::renderAircraftList(const std::vector<AircraftState>& aircraft,
                                         int selected_index,
                                         int scroll_offset) {
    // Draw list background
    fb_.fillRect(LIST_X, LIST_Y, LIST_W, LIST_H, COLOR_BG_DARK);

    // Draw list header
    drawText(LIST_X + 2, LIST_Y + 2, "ICAO", COLOR_TEXT_BRIGHT);
    drawText(LIST_X + 88 - 3 * 7, LIST_Y + 2, "ALT", COLOR_TEXT_BRIGHT);
    drawText(LIST_X + 122 - 4 * 7, LIST_Y + 2, "DIST", COLOR_TEXT_BRIGHT);

    // Draw horizontal line under header
    for (int x = LIST_X; x < LIST_X + LIST_W; x++) {
        fb_.drawPixel(x, LIST_Y + ROW_HEIGHT, COLOR_RADAR_GRID);
    }

    // Calculate visible range
    int start_idx = scroll_offset;
    int end_idx = std::min((int)aircraft.size(), scroll_offset + MAX_VISIBLE_ROWS - 1);

    // Draw aircraft rows
    for (int i = start_idx; i < end_idx; i++) {
        const auto& ac = aircraft[i];
        int row_y = LIST_Y + ROW_HEIGHT + (i - scroll_offset) * ROW_HEIGHT;

        // Highlight selected row
        bool is_selected = (i == selected_index);
        if (is_selected) {
            fb_.fillRect(LIST_X, row_y, LIST_W, ROW_HEIGHT, COLOR_SELECTED);
        }

        uint16_t text_color = is_selected ? COLOR_TEXT_BRIGHT : COLOR_TEXT_DIM;

        // Format and draw aircraft info
        char buffer[32];

        // ICAO (6 hex chars); the selected callsign is shown large below.
        snprintf(buffer, sizeof(buffer), "%06X", ac.icao_addr);
        drawText(LIST_X + 2, row_y + 2, buffer, text_color);

        // Altitude: 35k / 2.6k (right-aligned, ends at x=76)
        if (ac.position_valid && ac.altitude > 0) {
            if (ac.altitude >= 10000) {
                snprintf(buffer, sizeof(buffer), "%dk", ac.altitude / 1000);
            } else {
                snprintf(buffer, sizeof(buffer), "%.1fk", ac.altitude / 1000.0f);
            }
        } else {
            snprintf(buffer, sizeof(buffer), "-");
        }
        drawText(LIST_X + 88 - (int)strlen(buffer) * 7, row_y + 2, buffer, text_color);

        // Distance from the observer in km (right-aligned, ends at x=114)
        if (ac.distance > 0) {
            if (ac.distance < 10.0f) {
                snprintf(buffer, sizeof(buffer), "%.1f", ac.distance);
            } else {
                snprintf(buffer, sizeof(buffer), "%.0f", ac.distance);
            }
        } else {
            snprintf(buffer, sizeof(buffer), "-");
        }
        drawText(LIST_X + 122 - (int)strlen(buffer) * 7, row_y + 2, buffer, text_color);
    }

    // Draw scroll indicator if needed
    if (aircraft.size() > (size_t)MAX_VISIBLE_ROWS - 1) {
        int total_rows = aircraft.size();
        int visible_rows = MAX_VISIBLE_ROWS - 1;

        // Draw scroll bar on right edge
        int scrollbar_h = (LIST_H - ROW_HEIGHT) * visible_rows / total_rows;
        int scrollbar_y = LIST_Y + ROW_HEIGHT + (LIST_H - ROW_HEIGHT - scrollbar_h) * scroll_offset / (total_rows - visible_rows);

        for (int y = scrollbar_y; y < scrollbar_y + scrollbar_h; y++) {
            fb_.drawPixel(LIST_X + LIST_W - 2, y, COLOR_RADAR_TEXT);
        }
    }
}

void RadarUIRenderer::renderText(int x, int y, const char* text, uint16_t color, bool bold) {
    drawText(x, y, text, color);
}

void RadarUIRenderer::drawRangeRings(int cx, int cy, int radius, int num_rings, float max_range) {
    // Draw concentric circles for range rings in green
    for (int i = 1; i <= num_rings; i++) {
        int ring_radius = (radius * i) / num_rings;
        if (i < num_rings) {
            // Inner rings: dashed
            drawDashedCircleApprox(cx, cy, ring_radius, COLOR_RADAR_SWEEP);
        } else {
            // Outer ring: solid (acts as scale reference)
            drawCircleApprox(cx, cy, ring_radius, COLOR_RADAR_SWEEP);
        }
    }

    // Draw tick marks on outer ring every 15 degrees
    for (int angle = 0; angle < 360; angle += 15) {
        float rad = (angle - 90.0f) * M_PI / 180.0f;

        // Longer ticks at 30° intervals, shorter at 15°
        int tick_length = (angle % 30 == 0) ? 4 : 2;

        int x1 = cx + (int)((radius - tick_length) * cos(rad));
        int y1 = cy + (int)((radius - tick_length) * sin(rad));
        int x2 = cx + (int)(radius * cos(rad));
        int y2 = cy + (int)(radius * sin(rad));

        fb_.drawLine(x1, y1, x2, y2, COLOR_RADAR_SWEEP);
    }

}

void RadarUIRenderer::drawScaledText(int x, int y, const char* text,
                                     uint16_t color, int scale) {
    if (scale < 1) scale = 1;
    int cursor_x = x;
    for (const char* p = text; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t* glyph = font6x8[c - 0x20];
        for (int row = 0; row < FONT_6X8_HEIGHT; ++row) {
            const uint8_t line = glyph[row];
            for (int col = 0; col < FONT_6X8_WIDTH; ++col) {
                if (line & (0x80 >> col)) {
                    if (scale == 1) {
                        fb_.drawPixel(cursor_x + col, y + row, color);
                    } else {
                        fb_.fillRect(cursor_x + col * scale, y + row * scale,
                                     scale, scale, color);
                    }
                }
            }
        }
        cursor_x += (FONT_6X8_WIDTH + 1) * scale;
    }
}

void RadarUIRenderer::drawRingLabels(int cx, int cy, int radius, int num_rings,
                                     float max_range) {
    // Range labels sit on the south-west diagonal (225 deg), inside the ring,
    // away from the S cardinal label. Drawn after the sweep so they stay
    // readable while it passes.
    const float diag = 0.7071f;
    for (int i = 1; i <= num_rings; i++) {
        const int ring_radius = (radius * i) / num_rings;
        const float ring_range = (max_range * i) / num_rings;

        char label[8];
        snprintf(label, sizeof(label), "%.0f", ring_range);

        const int label_x = cx - (int)(ring_radius * diag) - 8;
        const int label_y = cy + (int)(ring_radius * diag) - 4;

        if (label_y > RADAR_Y && label_y < RADAR_Y + RADAR_H - 8) {
            drawText(label_x, label_y, label, COLOR_RADAR_TEXT);
        }
    }
}

void RadarUIRenderer::drawCompassMarkings(int cx, int cy, int radius) {
    // Cardinal labels sit *inside* the ring so they never collide with the
    // outer circle or the panel edges.
    drawText(cx - 3, cy - radius + 4, "N", COLOR_RADAR_TEXT);
    drawText(cx - 3, cy + radius - FONT_6X8_HEIGHT - 2, "S", COLOR_RADAR_TEXT);
    drawText(cx + radius - 10, cy - 4, "E", COLOR_RADAR_TEXT);
    drawText(cx - radius + 4, cy - 4, "W", COLOR_RADAR_TEXT);

    // Draw tick marks at cardinal directions
    for (int angle = 0; angle < 360; angle += 90) {
        float rad = (angle - 90.0f) * M_PI / 180.0f;
        int x1 = cx + (int)((radius - 3) * cos(rad));
        int y1 = cy + (int)((radius - 3) * sin(rad));
        int x2 = cx + (int)(radius * cos(rad));
        int y2 = cy + (int)(radius * sin(rad));
        fb_.drawLine(x1, y1, x2, y2, COLOR_RADAR_TEXT);
    }
}

void RadarUIRenderer::drawSweep(int cx, int cy, int radius, float angle) {
    // A single translucent wedge instead of a stack of discrete lines:
    // every pixel inside the circle is blended by its angular distance
    // behind the sweep, so the tail fades smoothly with no banding.
    constexpr float kTrailDeg = 50.0f;   // visible tail length
    constexpr float kInvDeg = 57.29578f; // 180/pi

    const int x0 = std::max(0, cx - radius);
    const int x1 = std::min(fb_.getWidth() - 1, cx + radius);
    const int y0 = std::max(0, cy - radius);
    const int y1 = std::min(fb_.getHeight() - 1, cy + radius);
    const int r2_max = radius * radius;

    for (int y = y0; y <= y1; ++y) {
        const int dy = cy - y;                 // screen y grows downwards
        const int dy2 = dy * dy;
        for (int x = x0; x <= x1; ++x) {
            const int dx = x - cx;
            if (dx * dx + dy2 > r2_max) continue;

            // Convert to the radar convention: 0 deg = north, clockwise.
            // math angle is counter-clockwise from +x, so radar = 90 - math.
            float pix_angle = 90.0f - atan2f((float)dy, (float)dx) * kInvDeg;
            if (pix_angle < 0.0f) pix_angle += 360.0f;

            float delta = angle - pix_angle;
            if (delta < 0.0f) delta += 360.0f;  // 0..360 behind the sweep
            if (delta > kTrailDeg) continue;

            const float t = 1.0f - delta / kTrailDeg;
            float alpha = t * t * 0.80f;        // quadratic fade
            if (delta < 1.0f) alpha = 0.95f;    // bright leading edge
            fb_.blendPixel(x, y, COLOR_RADAR_SWEEP, alpha);
        }
    }
}

void RadarUIRenderer::drawAircraftBlip(int cx, int cy, float distance, float bearing,
                                       float heading, bool velocity_valid,
                                       float max_range, bool selected) {
    // Convert polar to Cartesian
    int blip_x, blip_y;
    polarToCartesian(distance, bearing, max_range, &blip_x, &blip_y);

    // Choose color
    uint16_t color = selected ? COLOR_RADAR_SELECTED : COLOR_RADAR_BLIP;

    // Aircraft silhouette pattern (11 tall, 9 wide), pointing in +y direction.
    // Pointed nose (3 rows), tapering body, full-width wing at y=-1, narrow tail.
    // Each entry is (dx, dy) where dx = right offset, dy = forward offset.
    // No velocity data -> point North (0°).
    float h = velocity_valid ? heading : 0.0f;
    float rad = (h - 90.0f) * M_PI / 180.0f;
    float sin_h = sin(rad);
    float cos_h = cos(rad);

    // right perpendicular = (-sin_h, cos_h); forward = (cos_h, sin_h)
    static const int pattern[][2] = {
        { 0,  5},                                              // nose tip
        { 0,  4},                                              // nose
        { 0,  3},                                              // nose
        {-1,  2}, { 0,  2}, { 1,  2},                          // fuselage
        {-2,  1}, {-1,  1}, { 0,  1}, { 1,  1}, { 2,  1},     // fuselage
        {-3,  0}, {-2,  0}, {-1,  0}, { 0,  0},                // wing leading edge
        { 1,  0}, { 2,  0}, { 3,  0},                          // wing leading edge
        {-4, -1}, {-3, -1}, {-2, -1}, {-1, -1}, { 0, -1},      // wing full span
        { 1, -1}, { 2, -1}, { 3, -1}, { 4, -1},                // wing full span
        { 0, -2},                                              // fuselage
        { 0, -3},                                              // fuselage
        {-2, -4}, {-1, -4}, { 0, -4}, { 1, -4}, { 2, -4},      // tail
        { 0, -5},                                              // tail tip
    };
    constexpr int pattern_count = sizeof(pattern) / sizeof(pattern[0]);

    for (int i = 0; i < pattern_count; i++) {
        int dx = pattern[i][0];
        int dy = pattern[i][1];
        int sx = blip_x + (int)(dx * (-sin_h) + dy * cos_h);
        int sy = blip_y + (int)(dx * cos_h + dy * sin_h);
        fb_.drawPixel(sx, sy, color);
    }
}

void RadarUIRenderer::drawCircleApprox(int cx, int cy, int radius, uint16_t color) {
    // Bresenham's circle algorithm (outline only)
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

void RadarUIRenderer::drawDashedCircleApprox(int cx, int cy, int radius, uint16_t color) {
    // Dashed outline via parametric walk (~1 pixel of arc per step).
    // Dash pattern: 3 pixels on, 3 pixels off.
    const int dash_pixels = 3;
    const int gap_pixels = 3;
    if (radius <= 0) return;

    float step = 1.0f / (float)radius;
    if (step < 0.01f) step = 0.01f;

    int count = 0;
    bool drawing = true;

    for (float angle = 0.0f; angle < 2.0f * M_PI; angle += step) {
        if (drawing) {
            int x = cx + (int)(radius * cos(angle));
            int y = cy + (int)(radius * sin(angle));
            fb_.drawPixel(x, y, color);
        }

        count++;
        if (count >= (drawing ? dash_pixels : gap_pixels)) {
            drawing = !drawing;
            count = 0;
        }
    }
}

uint16_t RadarUIRenderer::blendColor(uint16_t color1, uint16_t color2, float alpha) {
    // Extract RGB components from RGB565
    uint8_t r1 = (color1 >> 11) & 0x1F;
    uint8_t g1 = (color1 >> 5) & 0x3F;
    uint8_t b1 = color1 & 0x1F;

    uint8_t r2 = (color2 >> 11) & 0x1F;
    uint8_t g2 = (color2 >> 5) & 0x3F;
    uint8_t b2 = color2 & 0x1F;

    // Blend components
    uint8_t r = (uint8_t)(r1 * alpha + r2 * (1.0f - alpha));
    uint8_t g = (uint8_t)(g1 * alpha + g2 * (1.0f - alpha));
    uint8_t b = (uint8_t)(b1 * alpha + b2 * (1.0f - alpha));

    // Combine back to RGB565
    return (r << 11) | (g << 5) | b;
}

void RadarUIRenderer::polarToCartesian(float distance, float bearing, float max_range,
                                       int* out_x, int* out_y) {
    // Normalize distance to radar radius
    float normalized_dist = (distance / max_range) * RADAR_RADIUS;

    // Clamp to radar radius
    if (normalized_dist > RADAR_RADIUS) {
        normalized_dist = RADAR_RADIUS;
    }

    // Convert bearing to radians (0° = North, clockwise)
    // Screen coordinates: 0° = up, 90° = right
    float angle_rad = (bearing - 90.0f) * M_PI / 180.0f;

    // Calculate screen position
    *out_x = RADAR_CENTER_X + (int)(normalized_dist * cos(angle_rad));
    *out_y = RADAR_CENTER_Y + (int)(normalized_dist * sin(angle_rad));
}
