#include "ui/aprs_ui_renderer.h"

#include "adsb/map_background.h"
#include "aprs/aprs_symbols.h"
#include "font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr int kScreenWidth = 320;
constexpr int kRowHeight = 18;
constexpr int kVisibleRows = 6;
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusNm = 3440.065;

void drawCircleOutline(Framebuffer& fb, int cx, int cy, int radius,
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

void drawDashedCircle(Framebuffer& fb, int cx, int cy, int radius,
                      uint16_t color) {
    for (int degrees = 0; degrees < 360; degrees += 8) {
        if ((degrees / 8) % 2 != 0) continue;
        const float a0 = degrees * static_cast<float>(kPi) / 180.0f;
        const float a1 = (degrees + 4) * static_cast<float>(kPi) / 180.0f;
        fb.drawLine(cx + static_cast<int>(std::lround(std::sin(a0) * radius)),
                    cy - static_cast<int>(std::lround(std::cos(a0) * radius)),
                    cx + static_cast<int>(std::lround(std::sin(a1) * radius)),
                    cy - static_cast<int>(std::lround(std::cos(a1) * radius)),
                    color);
    }
}

float distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double dlat = (lat2 - lat1) * kPi / 180.0;
    const double dlon = (lon2 - lon1) * kPi / 180.0;
    const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                     std::cos(lat1 * kPi / 180.0) *
                         std::cos(lat2 * kPi / 180.0) *
                         std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    return static_cast<float>(2.0 * kEarthRadiusNm *
                              std::asin(std::min(1.0, std::sqrt(a))));
}

float bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = lat1 * kPi / 180.0;
    const double phi2 = lat2 * kPi / 180.0;
    const double dlon = (lon2 - lon1) * kPi / 180.0;
    const double y = std::sin(dlon) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2) -
                     std::sin(phi1) * std::cos(phi2) * std::cos(dlon);
    double degrees = std::atan2(y, x) * 180.0 / kPi;
    if (degrees < 0.0) degrees += 360.0;
    return static_cast<float>(degrees);
}

void formatAge(time_t now, time_t seen, char* out, size_t size) {
    const long age = static_cast<long>(now - seen);
    if (age < 60) {
        std::snprintf(out, size, "%2lds", age);
    } else if (age < 3600) {
        std::snprintf(out, size, "%2ldm", age / 60);
    } else {
        std::snprintf(out, size, "%2ldh", age / 3600);
    }
}

void formatFrequency(uint32_t frequency, char* out, size_t size) {
    std::snprintf(out, size, "%u.%03u", frequency / 1000000u,
                  (frequency / 1000u) % 1000u);
}

uint16_t colorForKind(char kind, const uint16_t accent, const uint16_t dim,
                      const uint16_t alarm) {
    switch (kind) {
        case 'W': return 0x07FF;   // cyan
        case 'M': return 0xFFE0;   // amber
        case 'S': return dim;
        case 'T': return 0xF81F;   // magenta
        case 'O': return 0xFD20;   // orange
        default: return accent;    // position
    }
}

float trailLengthNm(const std::deque<aprs::TrailPoint>& trail) {
    float total = 0.0f;
    for (size_t i = 1; i < trail.size(); ++i) {
        total += distanceNm(trail[i - 1].latitude, trail[i - 1].longitude,
                            trail[i].latitude, trail[i].longitude);
    }
    return total;
}

void formatTrackDuration(long seconds, char* out, size_t size) {
    if (seconds < 60) {
        std::snprintf(out, size, "%ldSEC", seconds);
    } else if (seconds < 3600) {
        std::snprintf(out, size, "%ldMIN", seconds / 60);
    } else {
        std::snprintf(out, size, "%.1fHR", seconds / 3600.0);
    }
}

}  // namespace

AprsUIRenderer::AprsUIRenderer(Framebuffer& fb) : BaseUIRenderer(fb) {
    // Packet-terminal green on a near-black panel, amber selection.
    theme_ = {0x0841, 0x10A2, 0x1A65, 0xBFF7, 0x7CF0, 0x07E0,
              0x0328, 0xF800, 0x10A2, 0x07E0};
}

void AprsUIRenderer::drawChar(int x, int y, char c, uint16_t color) {
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t* glyph = font8x16[c - 0x20];
    for (int row = 0; row < FONT_8X16_HEIGHT; ++row) {
        const uint8_t line = glyph[row];
        for (int col = 0; col < FONT_8X16_WIDTH; ++col) {
            if (line & (0x80 >> col)) {
                fb_.drawPixel(x + col, y + row, color);
            }
        }
    }
}

void AprsUIRenderer::drawText(int x, int y, const char* text, uint16_t color) {
    if (!text) return;
    int cursor = x;
    while (*text) {
        drawChar(cursor, y, *text, color);
        cursor += FONT_8X16_WIDTH + 1;
        ++text;
    }
}

void AprsUIRenderer::drawTextBg(int x, int y, const char* text, uint16_t fg,
                                uint16_t bg) {
    if (!text) return;
    const int width = getTextWidth(text);
    fb_.fillRect(x, y, width, FONT_8X16_HEIGHT, bg);
    drawText(x, y, text, fg);
}

int AprsUIRenderer::getTextWidth(const char* text) const {
    return text ? static_cast<int>(std::strlen(text)) *
                      (FONT_8X16_WIDTH + 1)
                : 0;
}

void AprsUIRenderer::drawSmallText(int x, int y, const char* text,
                                   uint16_t color) {
    if (!text) return;
    int cursor = x;
    while (*text) {
        const char c = *text;
        if (c >= 0x20 && c <= 0x7E) {
            const uint8_t* glyph = font6x8[c - 0x20];
            for (int row = 0; row < FONT_6X8_HEIGHT; ++row) {
                const uint8_t line = glyph[row];
                for (int col = 0; col < FONT_6X8_WIDTH; ++col) {
                    if (line & (0x80 >> col)) {
                        fb_.drawPixel(cursor + col, y + row, color);
                    }
                }
            }
        }
        cursor += FONT_6X8_WIDTH + 1;
        ++text;
    }
}

int AprsUIRenderer::smallTextWidth(const char* text) const {
    return text ? static_cast<int>(std::strlen(text)) *
                      (FONT_6X8_WIDTH + 1)
                : 0;
}

void AprsUIRenderer::renderStatusBar(uint64_t total_packets,
                                     float packets_per_minute,
                                     int gain_tenths, bool agc,
                                     size_t stations, uint32_t frequency) {
    fb_.fillRect(0, 0, kScreenWidth, 20, theme_.status_bg);

    drawText(2, 2, "APRS", theme_.status_text);

    char buffer[32];
    formatFrequency(frequency, buffer, sizeof(buffer));
    drawText(46, 2, buffer, theme_.dim);
    drawText(112, 2, "RX", theme_.accent);

    std::snprintf(buffer, sizeof(buffer), "%zu STA", stations);
    drawText(138, 2, buffer, theme_.text);

    std::snprintf(buffer, sizeof(buffer), "%.1f/M", packets_per_minute);
    drawText(194, 2, buffer, theme_.dim);

    if (agc) {
        std::snprintf(buffer, sizeof(buffer), "AUTO");
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fDB", gain_tenths / 10.0f);
    }
    const int width = getTextWidth(buffer);
    drawText(kScreenWidth - width - 2, 2, buffer, theme_.status_text);
    (void)total_packets;

    fb_.fillRect(0, 19, kScreenWidth, 1, theme_.grid);
}

void AprsUIRenderer::renderStatsBar(uint64_t total_packets, size_t stations,
                                    int gain_tenths, bool agc,
                                    const char* hint) {
    fb_.fillRect(0, 152, kScreenWidth, 18, theme_.status_bg);
    fb_.fillRect(0, 152, kScreenWidth, 1, theme_.grid);

    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%llu PKT  %zu STA  RX ONLY",
                  static_cast<unsigned long long>(total_packets), stations);
    drawSmallText(2, 157, buffer, theme_.dim);
    (void)gain_tenths;
    (void)agc;

    drawSmallText(kScreenWidth - smallTextWidth(hint) - 2, 157, hint,
                  theme_.accent);
}

void AprsUIRenderer::renderList(
    const std::vector<aprs::StationState>& stations, int selected_index,
    int scroll_offset, uint64_t total_packets, float packets_per_minute,
    int gain_tenths, bool agc, uint32_t frequency) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_packets, packets_per_minute, gain_tenths, agc,
                    stations.size(), frequency);

    fb_.fillRect(0, 20, kScreenWidth, 18, theme_.panel);
    drawText(2, 21, "CALL", theme_.dim);
    drawText(86, 21, "TYP", theme_.dim);
    drawText(120, 21, "KN", theme_.dim);
    drawText(148, 21, "CRS", theme_.dim);
    drawText(196, 21, "NM", theme_.dim);
    drawText(252, 21, "AGE", theme_.dim);
    fb_.fillRect(0, 37, kScreenWidth, 1, theme_.grid);

    const int count = static_cast<int>(stations.size());
    const int first = std::max(0, std::min(scroll_offset,
                                           std::max(0, count - kVisibleRows)));
    const time_t now = time(nullptr);

    if (count == 0) {
        const char* waiting = "WAITING FOR APRS";
        drawText((kScreenWidth - getTextWidth(waiting)) / 2, 70, waiting,
                 theme_.accent);
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%.3f MHZ NFM",
                      frequency / 1e6);
        const int width = getTextWidth(buffer);
        drawText((kScreenWidth - width) / 2, 92, buffer, theme_.dim);
    }

    for (int row = 0; row < kVisibleRows; ++row) {
        const int index = first + row;
        if (index >= count) break;

        const aprs::StationState& station = stations[static_cast<size_t>(index)];
        const int y = 39 + row * kRowHeight;
        const bool selected = (index == selected_index);
        if (selected) {
            fb_.fillRect(0, y - 1, kScreenWidth, kRowHeight, theme_.selected);
            fb_.fillRect(0, y - 1, 2, kRowHeight, theme_.accent);
        } else if (row & 1) {
            fb_.fillRect(0, y - 1, kScreenWidth, kRowHeight, theme_.panel);
        }

        const uint16_t base = station.stale ? theme_.dim : theme_.text;
        const uint16_t text_color = selected ? 0xFFFF : base;
        const uint16_t accent_color = selected ? 0xFFFF
                                               : (station.stale
                                                      ? theme_.dim
                                                      : colorForKind(
                                                            station.kind,
                                                            theme_.accent,
                                                            theme_.dim,
                                                            theme_.alarm));

        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "%.9s", station.callsign.c_str());
        drawText(2, y, buffer, accent_color);

        std::snprintf(buffer, sizeof(buffer), "%s",
                      station.kind == 'W'
                          ? "WX"
                          : aprs::symbolLabel(station.symbol_table,
                                              station.symbol));
        drawText(86, y, buffer, text_color);

        if (station.speed_knots >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "%3.0f", station.speed_knots);
        } else {
            std::snprintf(buffer, sizeof(buffer), "  -");
        }
        drawText(118, y, buffer, text_color);

        if (station.course >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "%03.0f", station.course);
        } else {
            std::snprintf(buffer, sizeof(buffer), "---");
        }
        drawText(148, y, buffer, text_color);

        if (station.distance_nm >= 0.0f) {
            if (station.distance_nm < 100.0f) {
                std::snprintf(buffer, sizeof(buffer), "%4.1f",
                              station.distance_nm);
            } else {
                std::snprintf(buffer, sizeof(buffer), "%4.0f",
                              station.distance_nm);
            }
        } else {
            std::snprintf(buffer, sizeof(buffer), "  --");
        }
        drawText(196, y, buffer, text_color);

        formatAge(now, station.last_seen, buffer, sizeof(buffer));
        drawText(252, y, buffer, station.stale ? theme_.dim : text_color);
    }

    renderStatsBar(total_packets, stations.size(), gain_tenths, agc,
                   "H HELP");
}

void AprsUIRenderer::renderMap(
    const std::vector<aprs::StationState>& stations, int selected_index,
    float range_nm, bool station_valid, double station_latitude,
    double station_longitude, uint64_t total_packets,
    float packets_per_minute, int gain_tenths, bool agc, uint32_t frequency,
    MapBackground* map) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);

    constexpr int cx = 160;
    constexpr int cy = 88;
    constexpr int radius = 64;
    const bool has_map = (map != nullptr && station_valid);

    if (has_map) {
        map->draw(fb_, cx, cy, radius, range_nm * 1.852f, 0.0f);
    }

    renderStatusBar(total_packets, packets_per_minute, gain_tenths, agc,
                    stations.size(), frequency);
    char buffer[64];

    const uint16_t grid_color = has_map ? theme_.dim : theme_.grid;
    for (int ring = 1; ring <= 4; ++ring) {
        drawCircleOutline(fb_, cx, cy, radius * ring / 4,
                          ring == 4 ? (has_map ? theme_.accent : theme_.dim)
                                    : grid_color);
    }

    fb_.drawLine(cx - radius, cy, cx + radius, cy, grid_color);
    fb_.drawLine(cx, cy - radius, cx, cy + radius, grid_color);

    for (int degrees = 0; degrees < 360; degrees += 30) {
        const float angle = degrees * static_cast<float>(kPi) / 180.0f;
        const int inner = (degrees % 90 == 0) ? radius - 6 : radius - 3;
        const int x1 = cx + static_cast<int>(std::lround(std::sin(angle) * inner));
        const int y1 = cy - static_cast<int>(std::lround(std::cos(angle) * inner));
        const int x2 = cx + static_cast<int>(std::lround(std::sin(angle) * radius));
        const int y2 = cy - static_cast<int>(std::lround(std::cos(angle) * radius));
        fb_.drawLine(x1, y1, x2, y2, grid_color);
    }
    const uint16_t compass_color = has_map ? theme_.text : theme_.dim;
    drawSmallText(cx - 3, cy - radius + 3, "N", compass_color);
    drawSmallText(cx + radius - 9, cy - 4, "E", compass_color);
    drawSmallText(cx - 3, cy + radius - 11, "S", compass_color);
    drawSmallText(cx - radius + 3, cy - 4, "W", compass_color);

    // Own station: a small cross.
    fb_.drawLine(cx - 4, cy, cx + 4, cy, theme_.text);
    fb_.drawLine(cx, cy - 4, cx, cy + 4, theme_.text);
    fb_.drawPixel(cx, cy, theme_.accent);

    int drawn = 0;
    int selected_x = -1;
    int selected_y = -1;
    std::string selected_call;
    for (int index = 0; index < static_cast<int>(stations.size()); ++index) {
        const aprs::StationState& station =
            stations[static_cast<size_t>(index)];
        if (!station.has_position || station.distance_nm < 0.0f) continue;
        if (station.distance_nm > range_nm * 1.05f) continue;

        const int x = cx + static_cast<int>(std::lround(
            std::sin(station.bearing_deg * kPi / 180.0) *
            (station.distance_nm / range_nm) * radius));
        const int y = cy - static_cast<int>(std::lround(
            std::cos(station.bearing_deg * kPi / 180.0) *
            (station.distance_nm / range_nm) * radius));

        const bool selected = (index == selected_index);

        const auto project = [&](double latitude, double longitude, int& tx,
                                 int& ty) -> bool {
            const float trail_distance = distanceNm(
                station_latitude, station_longitude, latitude, longitude);
            if (trail_distance > range_nm) return false;
            const float trail_bearing = bearingDeg(
                station_latitude, station_longitude, latitude, longitude);
            tx = cx + static_cast<int>(std::lround(
                     std::sin(trail_bearing * kPi / 180.0) *
                     (trail_distance / range_nm) * radius));
            ty = cy - static_cast<int>(std::lround(
                     std::cos(trail_bearing * kPi / 180.0) *
                     (trail_distance / range_nm) * radius));
            return true;
        };

        // The selected station gets a connected, recent-weighted track.
        // Other stations keep faint dots so the map does not become busy.
        if (selected) {
            int previous_x = 0;
            int previous_y = 0;
            bool have_previous = false;
            const size_t count = station.trail.size();
            for (size_t t = 0; t < count; ++t) {
                int tx = 0;
                int ty = 0;
                if (!project(station.trail[t].latitude,
                             station.trail[t].longitude, tx, ty)) {
                    have_previous = false;
                    continue;
                }
                if (have_previous) {
                    const float fraction = static_cast<float>(t) /
                                           static_cast<float>(count - 1);
                    const uint16_t color = fraction > 0.75f
                                               ? 0xFD20
                                               : (fraction > 0.40f
                                                      ? theme_.accent
                                                      : theme_.dim);
                    fb_.drawLine(previous_x, previous_y, tx, ty, color);
                }
                previous_x = tx;
                previous_y = ty;
                have_previous = true;
            }
            int sx = 0;
            int sy = 0;
            if (!station.trail.empty() &&
                project(station.trail.front().latitude,
                        station.trail.front().longitude, sx, sy)) {
                drawCircleOutline(fb_, sx, sy, 2, theme_.text);
            }
        } else {
            for (size_t t = 0; t < station.trail.size(); t += 2) {
                int tx = 0;
                int ty = 0;
                if (project(station.trail[t].latitude,
                            station.trail[t].longitude, tx, ty)) {
                    fb_.drawPixel(tx, ty, theme_.grid);
                }
            }
        }
        const uint16_t color =
            selected ? 0xFD20
                     : (station.stale
                            ? theme_.dim
                            : colorForKind(station.kind, theme_.accent,
                                           theme_.dim, theme_.alarm));

        if (station.kind == 'W') {
            fb_.drawRect(x - 2, y - 2, 5, 5, color);
        } else if (station.kind == 'M') {
            drawCircleOutline(fb_, x, y, 2, color);
        } else if (station.course >= 0.0f && station.speed_knots > 0.5f) {
            const float course =
                station.course * static_cast<float>(kPi) / 180.0f;
            const int nose_x =
                x + static_cast<int>(std::lround(std::sin(course) * 4.0f));
            const int nose_y =
                y - static_cast<int>(std::lround(std::cos(course) * 4.0f));
            const int left_x = x + static_cast<int>(std::lround(
                std::sin(course + 2.5f) * 3.0f));
            const int left_y = y - static_cast<int>(std::lround(
                std::cos(course + 2.5f) * 3.0f));
            const int right_x = x + static_cast<int>(std::lround(
                std::sin(course - 2.5f) * 3.0f));
            const int right_y = y - static_cast<int>(std::lround(
                std::cos(course - 2.5f) * 3.0f));
            fb_.drawLine(nose_x, nose_y, left_x, left_y, color);
            fb_.drawLine(left_x, left_y, right_x, right_y, color);
            fb_.drawLine(right_x, right_y, nose_x, nose_y, color);
        } else {
            fb_.fillRect(x - 1, y - 1, 3, 3, color);
        }
        fb_.drawPixel(x, y, color);

        if (selected) {
            constexpr int r = 7;
            fb_.drawLine(x - r, y - r, x - r + 3, y - r, 0xFD20);
            fb_.drawLine(x - r, y - r, x - r, y - r + 3, 0xFD20);
            fb_.drawLine(x + r, y - r, x + r - 3, y - r, 0xFD20);
            fb_.drawLine(x + r, y - r, x + r, y - r + 3, 0xFD20);
            fb_.drawLine(x - r, y + r, x - r + 3, y + r, 0xFD20);
            fb_.drawLine(x - r, y + r, x - r, y + r - 3, 0xFD20);
            fb_.drawLine(x + r, y + r, x + r - 3, y + r, 0xFD20);
            fb_.drawLine(x + r, y + r, x + r, y + r - 3, 0xFD20);

            selected_x = x;
            selected_y = y;
            selected_call = station.callsign;
        }
        ++drawn;
    }

    // Draw the selected callsign last, so neighbouring symbols cannot cover
    // it on a crowded map.
    if (selected_x >= 0 && !selected_call.empty()) {
        const int label_y = std::max(22, selected_y - 13);
        const int label_w = smallTextWidth(selected_call.c_str());
        const int label_x = std::min(
            kScreenWidth - label_w - 2,
            std::max(2, selected_x - label_w / 2));
        fb_.fillRect(label_x - 1, label_y - 1, label_w + 2, 9, theme_.bg);
        drawSmallText(label_x, label_y, selected_call.c_str(), 0xFD20);
    }

    if (!station_valid) {
        drawText(78, 138, "POSITION NOT SET", theme_.alarm);
    } else if (drawn == 0) {
        drawText(86, 70, "WAITING FOR APRS", theme_.accent);
    }

    fb_.fillRect(0, 152, kScreenWidth, 18, theme_.status_bg);
    fb_.fillRect(0, 152, kScreenWidth, 1, theme_.grid);
    std::snprintf(buffer, sizeof(buffer), "PLOT %.0fNM", range_nm);
    drawSmallText(2, 157, buffer, theme_.text);
    if (selected_index >= 0 &&
        selected_index < static_cast<int>(stations.size())) {
        const aprs::StationState& selected =
            stations[static_cast<size_t>(selected_index)];
        if (selected.trail.size() >= 2) {
            std::snprintf(buffer, sizeof(buffer), "TRK %.1fNM",
                          trailLengthNm(selected.trail));
            drawSmallText(70, 157, buffer, theme_.dim);
        }
    }
    const char* hint = "UP/DN  <>  H HELP";
    drawSmallText(kScreenWidth - smallTextWidth(hint) - 2, 157, hint,
                  theme_.dim);
    (void)total_packets;
    (void)gain_tenths;
    (void)agc;
}

void AprsUIRenderer::renderTrackPlot(const aprs::StationState& station,
                                     int px, int py, int pw, int ph,
                                     MapBackground* map, double center_lat,
                                     double center_lon, float range_nm,
                                     double offset_east_m,
                                     double offset_north_m) {
    fb_.fillRect(px, py, pw, ph, theme_.panel);
    if (map != nullptr && station.has_position && range_nm > 0.0f) {
        map->drawRect(fb_, px, py, pw, ph, range_nm * 1.852f, 0.0f,
                      offset_east_m, offset_north_m);
    }
    fb_.drawRect(px, py, pw, ph, theme_.grid);

    const int title_width = smallTextWidth("TRACK") + 4;
    fb_.fillRect(px, py, title_width, 10, theme_.panel);
    drawSmallText(px + 4, py + 3, "TRACK", theme_.dim);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%zu", station.trail.size());
    const int count_width = smallTextWidth(buffer) + 4;
    fb_.fillRect(px + pw - count_width, py, count_width, 10, theme_.panel);
    drawSmallText(px + pw - smallTextWidth(buffer) - 4, py + 3, buffer,
                  theme_.dim);

    const int ix = px + 4;
    const int iy = py + 13;
    const int iw = pw - 8;
    const int ih = ph - 16;
    const bool has_map =
        map != nullptr && station.has_position && range_nm > 0.0f;

    // The grid is only a fallback for the no-map case. Over a real map it
    // hides roads, labels and coastlines on the small panel.
    if (!has_map) {
        for (int i = 1; i < 4; ++i) {
            fb_.drawLine(ix + iw * i / 4, iy, ix + iw * i / 4, iy + ih,
                         theme_.grid);
            fb_.drawLine(ix, iy + ih * i / 4, ix + iw, iy + ih * i / 4,
                         theme_.grid);
        }
    }

    const auto draw_compass = [&](int x, int y, const char* text) {
        if (has_map) {
            fb_.fillRect(x - 1, y - 1, smallTextWidth(text) + 2, 9,
                         theme_.panel);
        }
        drawSmallText(x, y, text, has_map ? theme_.text : theme_.dim);
    };
    draw_compass(ix + iw / 2 - 2, iy + 1, "N");
    draw_compass(ix + iw - 7, iy + ih / 2 - 3, "E");

    if (range_nm <= 0.0f && station.has_position) {
        center_lat = station.latitude;
        center_lon = station.longitude;
        range_nm = 1.0f;
    }
    const double cos_latitude = std::cos(center_lat * kPi / 180.0);
    const double half = std::min(pw, ph) / 2.0;
    const double pixels_per_nm =
        range_nm > 0.0f ? half / range_nm : 1.0;
    const int cx = px + pw / 2;
    const int cy = py + ph / 2;

    const auto project = [&](const aprs::TrailPoint& point, int& x, int& y) {
        x = cx + static_cast<int>(std::lround(
                     (point.longitude - center_lon) * 60.0 * cos_latitude *
                     pixels_per_nm));
        y = cy - static_cast<int>(std::lround(
                     (point.latitude - center_lat) * 60.0 * pixels_per_nm));
    };
    const int clip_left = px + 2;
    const int clip_right = px + pw - 3;
    const int clip_top = py + 13;
    const int clip_bottom = py + ph - 3;
    const auto inside = [&](int x, int y, int margin = 0) {
        return x >= clip_left + margin && x <= clip_right - margin &&
               y >= clip_top + margin && y <= clip_bottom - margin;
    };

    if (station.trail.size() < 2) {
        const char* message = "NO TRACK YET";
        const int message_width = smallTextWidth(message) + 4;
        const int message_x = px + (pw - message_width) / 2;
        const int message_y = py + ph - 15;
        fb_.fillRect(message_x, message_y, message_width, 10, theme_.panel);
        drawSmallText(message_x + 2, message_y + 1, message, theme_.dim);
        if (station.has_position) {
            aprs::TrailPoint point;
            point.latitude = station.latitude;
            point.longitude = station.longitude;
            int x = 0;
            int y = 0;
            project(point, x, y);
            if (inside(x, y, 4)) {
                fb_.drawLine(x - 3, y, x + 3, y, theme_.text);
                fb_.drawLine(x, y - 3, x, y + 3, theme_.text);
                fb_.drawPixel(x, y, 0xFD20);
            }
        }
        return;
    }

    int previous_x = 0;
    int previous_y = 0;
    bool have_previous = false;
    const size_t count = station.trail.size();
    for (size_t i = 0; i < count; ++i) {
        int x = 0;
        int y = 0;
        project(station.trail[i], x, y);
        if (!inside(x, y)) {
            have_previous = false;
            continue;
        }
        if (have_previous) {
            const float fraction = static_cast<float>(i) /
                                   static_cast<float>(count - 1);
            const uint16_t color = fraction > 0.75f
                                       ? 0xFD20
                                       : (fraction > 0.40f ? theme_.accent
                                                           : theme_.dim);
            fb_.drawLine(previous_x, previous_y, x, y, color);
        }
        previous_x = x;
        previous_y = y;
        have_previous = true;
        if (i + 8 >= count) fb_.drawPixel(x, y, theme_.text);
    }

    int start_x = 0;
    int start_y = 0;
    project(station.trail.front(), start_x, start_y);
    if (inside(start_x, start_y, 3)) {
        drawCircleOutline(fb_, start_x, start_y, 2, theme_.text);
    }

    int end_x = 0;
    int end_y = 0;
    project(station.trail.back(), end_x, end_y);
    const float course = station.course >= 0.0f
                             ? station.course
                             : station.trail.back().course;
    if (!inside(end_x, end_y, 5)) {
        return;
    }
    if (course >= 0.0f) {
        const float angle = course * static_cast<float>(kPi) / 180.0f;
        const int nose_x =
            end_x + static_cast<int>(std::lround(std::sin(angle) * 4.0f));
        const int nose_y =
            end_y - static_cast<int>(std::lround(std::cos(angle) * 4.0f));
        const int left_x = end_x + static_cast<int>(std::lround(
            std::sin(angle + 2.5f) * 3.0f));
        const int left_y = end_y - static_cast<int>(std::lround(
            std::cos(angle + 2.5f) * 3.0f));
        const int right_x = end_x + static_cast<int>(std::lround(
            std::sin(angle - 2.5f) * 3.0f));
        const int right_y = end_y - static_cast<int>(std::lround(
            std::cos(angle - 2.5f) * 3.0f));
        fb_.drawLine(nose_x, nose_y, left_x, left_y, 0xFD20);
        fb_.drawLine(left_x, left_y, right_x, right_y, 0xFD20);
        fb_.drawLine(right_x, right_y, nose_x, nose_y, 0xFD20);
    } else {
        fb_.fillRect(end_x - 1, end_y - 1, 3, 3, 0xFD20);
    }
}

void AprsUIRenderer::renderDetail(
    const aprs::StationState* station, size_t station_count,
    uint64_t total_packets, float packets_per_minute, int gain_tenths,
    bool agc, int& scroll, uint32_t frequency, MapBackground* map,
    double map_center_lat, double map_center_lon, float map_range_nm,
    double map_offset_east_m, double map_offset_north_m) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_packets, packets_per_minute, gain_tenths, agc,
                    station_count, frequency);

    if (station == nullptr) {
        drawText(64, 70, "NO STATION SELECTED", theme_.dim);
        drawSmallText(2, 161, "ENTER LIST  H HELP", theme_.dim);
        scroll = 0;
        return;
    }

    constexpr int plot_x = 2;
    constexpr int plot_y = 24;
    constexpr int plot_w = 160;
    constexpr int plot_h = 124;
    renderTrackPlot(*station, plot_x, plot_y, plot_w, plot_h, map,
                    map_center_lat, map_center_lon, map_range_nm,
                    map_offset_east_m, map_offset_north_m);

    char buffer[96];
    if (station->trail.size() >= 2) {
        const float length = trailLengthNm(station->trail);
        const long duration = static_cast<long>(
            station->trail.back().time - station->trail.front().time);
        char span[16];
        formatTrackDuration(std::max(0L, duration), span, sizeof(span));
        std::snprintf(buffer, sizeof(buffer), "TRK %.1fNM %s %zuPTS", length,
                      span, station->trail.size());
        drawSmallText(plot_x, 151, buffer, theme_.text);
    } else if (station->trail.size() == 1) {
        drawSmallText(plot_x, 151, "TRK 1 FIX", theme_.dim);
    } else {
        drawSmallText(plot_x, 151, "TRK --", theme_.dim);
    }

    fb_.fillRect(plot_x + plot_w + 2, 22, 1, 126, theme_.grid);

    const int rx = plot_x + plot_w + 6;
    drawText(rx, 24, station->callsign.c_str(), theme_.accent);

    char kind_line[32];
    std::snprintf(kind_line, sizeof(kind_line), "%s %s",
                  aprs::Store::kindLabel(station->kind),
                  aprs::symbolLabel(station->symbol_table, station->symbol));
    drawSmallText(rx, 42, kind_line, theme_.text);

    struct Line {
        std::string text;
        uint16_t color;
    };
    std::vector<Line> lines;
    const auto add = [&](const std::string& text, uint16_t color) {
        if (!text.empty()) lines.push_back({text, color});
    };
    const auto add_wrapped = [&](const std::string& text, size_t width,
                                 uint16_t color) {
        size_t offset = 0;
        while (offset < text.size()) {
            std::string line = text.substr(offset, width);
            offset += line.size();
            if (offset < text.size()) {
                const size_t space = line.rfind(' ');
                if (space != std::string::npos && space > 4) {
                    offset -= line.size() - space - 1;
                    line.resize(space);
                }
            }
            add(line, color);
        }
    };

    if (station->has_position) {
        std::snprintf(buffer, sizeof(buffer), "LAT %.5f %c",
                      std::fabs(station->latitude),
                      station->latitude >= 0.0 ? 'N' : 'S');
    } else {
        std::snprintf(buffer, sizeof(buffer), "LAT --");
    }
    add(buffer, theme_.text);

    if (station->has_position) {
        std::snprintf(buffer, sizeof(buffer), "LON %.5f %c",
                      std::fabs(station->longitude),
                      station->longitude >= 0.0 ? 'E' : 'W');
    } else {
        std::snprintf(buffer, sizeof(buffer), "LON --");
    }
    add(buffer, theme_.text);

    if (station->has_position && station->distance_nm >= 0.0f) {
        std::snprintf(buffer, sizeof(buffer), "RNG %.1fNM BRG%03.0fDEG",
                      station->distance_nm, station->bearing_deg);
    } else {
        std::snprintf(buffer, sizeof(buffer), "RNG --");
    }
    add(buffer, theme_.text);

    char speed[8];
    char course[10];
    if (station->speed_knots >= 0.0f) {
        std::snprintf(speed, sizeof(speed), "%.0fKN", station->speed_knots);
    } else {
        std::snprintf(speed, sizeof(speed), "--");
    }
    if (station->course >= 0.0f) {
        std::snprintf(course, sizeof(course), "%03.0fDEG", station->course);
    } else {
        std::snprintf(course, sizeof(course), "--");
    }
    std::snprintf(buffer, sizeof(buffer), "SPD %s CRS %s", speed, course);
    add(buffer, theme_.text);

    if (station->altitude_ft > -1000) {
        std::snprintf(buffer, sizeof(buffer), "ALT %dFT",
                      station->altitude_ft);
    } else {
        std::snprintf(buffer, sizeof(buffer), "ALT --");
    }
    add(buffer, theme_.text);

    const long age = static_cast<long>(time(nullptr) - station->last_seen);
    std::snprintf(buffer, sizeof(buffer), "AGE %ldS PKT%u", age,
                  station->packets);
    add(buffer, theme_.dim);

    std::snprintf(buffer, sizeof(buffer), "TYPE %s",
                  station->kind == 'W'
                      ? "Weather station"
                      : aprs::symbolName(station->symbol_table,
                                         station->symbol));
    add(buffer, theme_.dim);

    std::snprintf(buffer, sizeof(buffer), "DST %.13s",
                  station->destination.empty() ? "--"
                                               : station->destination.c_str());
    add(buffer, theme_.dim);

    std::string via = "VIA ";
    via += station->path.empty() ? "DIRECT" : station->path;
    add_wrapped(via, 21, theme_.dim);

    if (station->weather) {
        if (station->wind_dir >= 0.0f && station->wind_speed_mph >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "WIND %03.0fDEG %.0fMPH",
                          station->wind_dir, station->wind_speed_mph);
        } else {
            std::snprintf(buffer, sizeof(buffer), "WIND --");
        }
        add(buffer, 0x07FF);

        if (station->wind_gust_mph >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "GUST %.0fMPH",
                          station->wind_gust_mph);
        } else {
            std::snprintf(buffer, sizeof(buffer), "GUST --");
        }
        add(buffer, 0x07FF);

        if (station->has_temperature) {
            std::snprintf(buffer, sizeof(buffer), "TEMP %.0fC",
                          station->temperature_c);
        } else {
            std::snprintf(buffer, sizeof(buffer), "TEMP --");
        }
        add(buffer, 0x07FF);

        if (station->humidity >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "HUM %.0f%%",
                          station->humidity);
        } else {
            std::snprintf(buffer, sizeof(buffer), "HUM --");
        }
        add(buffer, 0x07FF);

        if (station->pressure_hpa >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "PRES %.1fHPA",
                          station->pressure_hpa);
        } else {
            std::snprintf(buffer, sizeof(buffer), "PRES --");
        }
        add(buffer, 0x07FF);

        if (station->rain_hour_mm >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "RAIN1H %.1fMM",
                          station->rain_hour_mm);
        } else {
            std::snprintf(buffer, sizeof(buffer), "RAIN1H --");
        }
        add(buffer, 0x07FF);

        if (station->rain_24h_mm >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "RAIN24H %.1fMM",
                          station->rain_24h_mm);
        } else {
            std::snprintf(buffer, sizeof(buffer), "RAIN24H --");
        }
        add(buffer, 0x07FF);

        if (station->rain_midnight_mm >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "RAINMID %.1fMM",
                          station->rain_midnight_mm);
        } else {
            std::snprintf(buffer, sizeof(buffer), "RAINMID --");
        }
        add(buffer, 0x07FF);
    }

    if (!station->comment.empty()) {
        add("COMMENT", theme_.dim);
        add_wrapped(station->comment, 21, theme_.text);
    }

    constexpr int kVisibleLines = 11;
    const int max_scroll =
        std::max(0, static_cast<int>(lines.size()) - kVisibleLines);
    scroll = std::max(0, std::min(scroll, max_scroll));
    for (int i = 0; i < kVisibleLines; ++i) {
        const int index = scroll + i;
        if (index >= static_cast<int>(lines.size())) break;
        drawSmallText(rx, 54 + i * 9, lines[static_cast<size_t>(index)].text.c_str(),
                      lines[static_cast<size_t>(index)].color);
    }

    if (max_scroll > 0) {
        char indicator[16];
        std::snprintf(indicator, sizeof(indicator), "%d/%zu", scroll + 1,
                      lines.size());
        drawSmallText(kScreenWidth - smallTextWidth(indicator) - 2, 151,
                      indicator, theme_.dim);
    }

    drawSmallText(2, 161, "UP/DN SCROLL  <> STATION  H HELP", theme_.dim);
}

void AprsUIRenderer::renderConsole(
    const std::vector<aprs::LogEntry>& entries, int scroll_offset,
    uint64_t total_packets, float packets_per_minute, int gain_tenths,
    bool agc, size_t station_count, uint32_t frequency) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_packets, packets_per_minute, gain_tenths, agc,
                    station_count, frequency);
    fb_.fillRect(0, 20, kScreenWidth, 1, theme_.grid);

    constexpr int kVisibleEntries = 8;
    const int total = static_cast<int>(entries.size());
    const int end = std::max(0, total - scroll_offset);
    const int start = std::max(0, end - kVisibleEntries);

    if (total == 0) {
        drawText(86, 74, "WAITING FOR APRS", theme_.accent);
    } else {
        for (int index = start; index < end; ++index) {
            const int row = index - start;
            const int y = 24 + row * 16;
            const aprs::LogEntry& entry = entries[static_cast<size_t>(index)];
            const bool newest = (index == total - 1);

            struct tm local {};
            localtime_r(&entry.time, &local);
            char prefix[40];
            std::snprintf(prefix, sizeof(prefix), "%02d:%02d:%02d %-9s %c",
                          local.tm_hour, local.tm_min, local.tm_sec,
                          entry.source.c_str(), entry.kind);
            drawSmallText(2, y, prefix,
                          newest ? theme_.text : theme_.dim);

            const std::string& summary =
                entry.summary.empty() ? entry.text : entry.summary;
            char clipped[64];
            std::snprintf(clipped, sizeof(clipped), "%.44s",
                          summary.c_str());
            drawSmallText(8, y + 8, clipped,
                          newest ? theme_.accent : theme_.dim);
        }
    }

    renderStatsBar(total_packets, station_count, gain_tenths, agc,
                   "UP/DN  ENTER");
}

void AprsUIRenderer::renderHelp() {
    fb_.fillRect(10, 22, 300, 126, theme_.panel);
    fb_.fillRect(10, 22, 300, 1, theme_.accent);
    fb_.fillRect(10, 147, 300, 1, theme_.accent);
    fb_.fillRect(10, 22, 1, 126, theme_.accent);
    fb_.fillRect(309, 22, 1, 126, theme_.accent);

    drawText(20, 28, "APRS KEYS  RX ONLY", theme_.accent);
    const char* lines[] = {
        "ENTER  LIST / MAP / DETAIL / CONSOLE",
        "UP/DN  SCROLL / ZOOM MAP",
        "LEFT/RIGHT  SELECT STATION",
        "+/-    GAIN / DETAIL TEXT",
        "G      AUTO / MANUAL",
        "H      HELP",
        "T      CYCLE APRS FREQ",
        "FN+P   SCREENSHOT",
        "TAB    PAGES",
        "ESC    LIST / CLOSE",
    };
    for (int i = 0; i < 10; ++i) {
        drawSmallText(22, 46 + i * 10, lines[i],
                      i == 0 ? theme_.text : theme_.dim);
    }
}
