#include "ui/aprs_ui_renderer.h"

#include "adsb/map_background.h"
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
    drawText(88, 21, "SYM", theme_.dim);
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

        const char symbol =
            (station.symbol >= 0x20 && station.symbol <= 0x7E)
                ? station.symbol
                : '?';
        std::snprintf(buffer, sizeof(buffer), "%c", symbol);
        drawText(94, y, buffer, text_color);

        if (station.speed_knots >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "%3.0f", station.speed_knots);
        } else {
            std::snprintf(buffer, sizeof(buffer), "  -");
        }
        drawText(108, y, buffer, text_color);

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

        // Trail, oldest first.
        for (size_t t = 0; t < station.trail.size(); ++t) {
            const float trail_distance =
                distanceNm(station_latitude, station_longitude,
                           station.trail[t].first, station.trail[t].second);
            if (trail_distance > range_nm) continue;
            const float trail_bearing =
                bearingDeg(station_latitude, station_longitude,
                           station.trail[t].first, station.trail[t].second);
            const int tx = cx + static_cast<int>(std::lround(
                std::sin(trail_bearing * kPi / 180.0) *
                (trail_distance / range_nm) * radius));
            const int ty = cy - static_cast<int>(std::lround(
                std::cos(trail_bearing * kPi / 180.0) *
                (trail_distance / range_nm) * radius));
            fb_.drawPixel(tx, ty,
                          t + 2 >= station.trail.size() ? theme_.dim
                                                        : theme_.grid);
        }

        const bool selected = (index == selected_index);
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
    const char* hint = "UP/DN RANGE  <> TGT  H HELP";
    drawSmallText(kScreenWidth - smallTextWidth(hint) - 2, 157, hint,
                  theme_.dim);
    (void)total_packets;
    (void)gain_tenths;
    (void)agc;
}

void AprsUIRenderer::renderDetail(
    const aprs::StationState* station, size_t station_count,
    uint64_t total_packets, float packets_per_minute, int gain_tenths,
    bool agc, uint32_t frequency) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_packets, packets_per_minute, gain_tenths, agc,
                    station_count, frequency);

    if (station == nullptr) {
        drawText(64, 70, "NO STATION SELECTED", theme_.dim);
        renderStatsBar(total_packets, station_count, gain_tenths, agc,
                       "H HELP");
        return;
    }

    char buffer[96];
    drawText(6, 24, station->callsign.c_str(), theme_.accent);
    const char* kind = aprs::Store::kindLabel(station->kind);
    const int kind_width = smallTextWidth(kind);
    drawSmallText(kScreenWidth - kind_width - 6, 29, kind, theme_.text);

    char symbol[8];
    std::snprintf(symbol, sizeof(symbol), "%c",
                  (station->symbol >= 0x20 && station->symbol <= 0x7E)
                      ? station->symbol
                      : '?');
    drawSmallText(kScreenWidth - kind_width - 24, 29, symbol, theme_.dim);

    fb_.fillRect(6, 44, kScreenWidth - 12, 1, theme_.grid);

    int y = 50;
    if (station->has_position) {
        std::snprintf(buffer, sizeof(buffer), "POS  %.5f, %.5f",
                      station->latitude, station->longitude);
    } else {
        std::snprintf(buffer, sizeof(buffer), "POS  --");
    }
    drawSmallText(6, y, buffer, theme_.text);
    y += 10;

    if (station->has_position && station->distance_nm >= 0.0f) {
        std::snprintf(buffer, sizeof(buffer), "RNG  %.1f NM   BRG %03.0f",
                      station->distance_nm, station->bearing_deg);
    } else {
        std::snprintf(buffer, sizeof(buffer), "RNG  --");
    }
    drawSmallText(6, y, buffer, theme_.text);
    y += 10;

    char speed[16];
    char course[16];
    if (station->speed_knots >= 0.0f) {
        std::snprintf(speed, sizeof(speed), "%.0f kn", station->speed_knots);
    } else {
        std::snprintf(speed, sizeof(speed), "--");
    }
    if (station->course >= 0.0f) {
        std::snprintf(course, sizeof(course), "%03.0f", station->course);
    } else {
        std::snprintf(course, sizeof(course), "---");
    }
    std::snprintf(buffer, sizeof(buffer), "SPD  %-8s CRS %s", speed, course);
    drawSmallText(6, y, buffer, theme_.text);
    y += 10;

    if (station->altitude_ft > -1000) {
        std::snprintf(buffer, sizeof(buffer), "ALT  %d ft",
                      station->altitude_ft);
    } else {
        std::snprintf(buffer, sizeof(buffer), "ALT  --");
    }
    drawSmallText(6, y, buffer, theme_.text);
    y += 10;

    const long age = static_cast<long>(time(nullptr) - station->last_seen);
    std::snprintf(buffer, sizeof(buffer), "AGE  %lds   PKT %u", age,
                  station->packets);
    drawSmallText(6, y, buffer, theme_.dim);
    y += 10;

    std::snprintf(buffer, sizeof(buffer), "DST  %.9s   VIA %.20s",
                  station->destination.empty() ? "--"
                                               : station->destination.c_str(),
                  station->path.empty() ? "DIRECT" : station->path.c_str());
    drawSmallText(6, y, buffer, theme_.dim);
    y += 10;

    if (station->weather) {
        char wind[24];
        if (station->wind_dir >= 0.0f) {
            std::snprintf(wind, sizeof(wind), "%03.0f/%.0f mph",
                          station->wind_dir, station->wind_speed_mph);
        } else {
            std::snprintf(wind, sizeof(wind), "--");
        }
        std::snprintf(buffer, sizeof(buffer), "WX   WIND %s  TEMP %.1fC",
                      wind, station->temperature_c);
        drawSmallText(6, y, buffer, 0x07FF);
        y += 10;
        if (station->rain_hour_mm >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "     RAIN %.1f mm/h",
                          station->rain_hour_mm);
            drawSmallText(6, y, buffer, 0x07FF);
            y += 10;
        }
    }

    // Comment, wrapped over up to three lines.
    const std::string& comment = station->comment;
    if (!comment.empty() && y <= 130) {
        constexpr size_t kWidth = 44;
        size_t offset = 0;
        int lines = 0;
        while (offset < comment.size() && lines < 3) {
            std::string line = comment.substr(offset, kWidth);
            offset += line.size();
            drawSmallText(6, y, line.c_str(),
                          lines == 0 ? theme_.text : theme_.dim);
            y += 10;
            ++lines;
        }
    }

    renderStatsBar(total_packets, station_count, gain_tenths, agc,
                   "H HELP");
}

void AprsUIRenderer::renderConsole(
    const std::vector<aprs::LogEntry>& entries, int scroll_offset,
    uint64_t total_packets, float packets_per_minute, int gain_tenths,
    bool agc, size_t station_count, uint32_t frequency) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_packets, packets_per_minute, gain_tenths, agc,
                    station_count, frequency);
    fb_.fillRect(0, 20, kScreenWidth, 1, theme_.grid);

    constexpr int kVisibleLines = 16;
    const int total = static_cast<int>(entries.size());
    const int end = std::max(0, total - scroll_offset);
    const int start = std::max(0, end - kVisibleLines);
    const int lines = end - start;

    if (total == 0) {
        drawText(86, 74, "WAITING FOR APRS", theme_.accent);
    } else {
        for (int i = 0; i < lines; ++i) {
            const int index = start + i;
            const int y = 24 + i * 8;
            const aprs::LogEntry& entry = entries[static_cast<size_t>(index)];
            const bool newest = (index == total - 1);
            const char* text = entry.text.c_str();
            char clipped[46];
            std::snprintf(clipped, sizeof(clipped), "%.45s", text);
            drawSmallText(2, y, clipped,
                          newest ? theme_.text : theme_.dim);
        }
    }

    renderStatsBar(total_packets, station_count, gain_tenths, agc,
                   "ENTER LIST");
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
        "UP/DN  SCROLL / RANGE / TARGET",
        "LEFT/RIGHT  SELECT STATION",
        "+/-    GAIN",
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
