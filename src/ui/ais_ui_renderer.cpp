#include "ui/ais_ui_renderer.h"

#include "font.h"
#include "adsb/map_background.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr int kScreenWidth = 320;
constexpr int kRowHeight = 18;
constexpr int kVisibleRows = 6;

const char* vesselMarker(const ais::VesselState& vessel) {
    if (vessel.aton) return "A";
    if (vessel.base_station) return "S";
    if (vessel.class_b) return "B";
    switch (vessel.ship_type) {
        case 30: return "F";
        case 31:
        case 32:
        case 52: return "U";
        case 36:
        case 37: return "Y";
        case 50: return "P";
        case 51: return "R";
        case 55: return "L";
        default: break;
    }
    if (vessel.ship_type >= 60 && vessel.ship_type < 70) return "P";
    if (vessel.ship_type >= 70 && vessel.ship_type < 80) return "C";
    if (vessel.ship_type >= 80 && vessel.ship_type < 90) return "T";
    return "?";
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

}  // namespace

namespace {

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

void drawTargetTriangle(Framebuffer& fb, int cx, int cy, float heading,
                        uint16_t color, int size) {
    const float angle = heading * static_cast<float>(kPi) / 180.0f;
    const int nose_x = cx + static_cast<int>(std::lround(std::sin(angle) * size));
    const int nose_y = cy - static_cast<int>(std::lround(std::cos(angle) * size));
    const int left_x = cx + static_cast<int>(std::lround(std::sin(angle + 2.4f) * size));
    const int left_y = cy - static_cast<int>(std::lround(std::cos(angle + 2.4f) * size));
    const int right_x = cx + static_cast<int>(std::lround(std::sin(angle - 2.4f) * size));
    const int right_y = cy - static_cast<int>(std::lround(std::cos(angle - 2.4f) * size));
    fb.drawLine(nose_x, nose_y, left_x, left_y, color);
    fb.drawLine(left_x, left_y, right_x, right_y, color);
    fb.drawLine(right_x, right_y, nose_x, nose_y, color);
}

const char* navStatusLabel(int status) {
    switch (status) {
        case 0: return "Under way";
        case 1: return "At anchor";
        case 2: return "Not under command";
        case 3: return "Restricted";
        case 4: return "Draft limited";
        case 5: return "Moored";
        case 6: return "Aground";
        case 7: return "Fishing";
        case 8: return "Sailing";
        case 15: return "Undefined";
        default: return "Unknown";
    }
}

void drawShipSilhouette(Framebuffer& fb, int x, int y, int w, int h,
                        const ais::VesselState& vessel, uint16_t color) {
    if (vessel.base_station) {
        const int cx = x + w / 2;
        fb.drawLine(cx, y + 20, cx, y + h - 10, color);
        fb.drawLine(cx, y + 20, cx - 12, y + 40, color);
        fb.drawLine(cx, y + 20, cx + 12, y + 40, color);
        fb.drawLine(cx - 14, y + h - 10, cx + 14, y + h - 10, color);
        fb.drawLine(cx - 5, y + 48, cx + 5, y + 48, color);
        return;
    }
    if (vessel.aton) {
        const int cx = x + w / 2;
        const int cy = y + h / 2 + 6;
        drawCircleOutline(fb, cx, cy, 12, color);
        fb.drawLine(cx, cy - 26, cx, cy - 12, color);
        fb.drawLine(cx - 6, cy + 12, cx + 6, cy + 12, color);
        fb.drawLine(cx, cy + 12, cx, cy + 22, color);
        fb.drawLine(x + 10, cy + 22, x + w - 10, cy + 22, color);
        return;
    }

    const int hull_top = y + h * 55 / 100;
    const int hull_bottom = y + h * 80 / 100;
    const int hull_span = std::max(1, hull_bottom - hull_top + 1);
    for (int yy = hull_top; yy <= hull_bottom; ++yy) {
        const int inset = (yy - hull_top) * (w / 20) / hull_span;
        fb.drawLine(x + inset, yy, x + w - inset, yy, color);
    }
    fb.drawLine(x + w - 2, hull_top + 2, x + w - 2, hull_bottom - 2, color);
    fb.drawLine(x, y + h * 84 / 100, x + w, y + h * 84 / 100, 0x2945);

    const int type = vessel.ship_type;
    if ((vessel.class_b && (type == 36 || type == 37)) || type == 36 ||
        type == 37) {
        const int mx = x + w / 2;
        fb.drawLine(mx, hull_top, mx, y + 8, color);
        fb.drawLine(mx, y + 10, mx + 24, hull_top - 4, color);
        fb.drawLine(mx + 24, hull_top - 4, mx, hull_top, color);
        fb.drawLine(mx, y + 10, mx - 18, hull_top - 2, color);
        fb.drawLine(mx - 18, hull_top - 2, mx, hull_top, color);
    } else if (type == 30) {
        fb.fillRect(x + w * 55 / 100, hull_top - 14, w * 22 / 100, 14, color);
        fb.drawLine(x + w * 40 / 100, hull_top, x + w * 40 / 100, y + 12, color);
        fb.drawLine(x + w * 40 / 100, y + 12, x + w * 62 / 100, y + 12, color);
    } else if (type >= 60 && type < 70) {
        fb.fillRect(x + w * 45 / 100, hull_top - 18, w * 35 / 100, 18, color);
        fb.fillRect(x + w * 30 / 100, hull_top - 32, w * 50 / 100, 14, color);
        for (int wx = x + w * 33 / 100; wx < x + w * 77 / 100; wx += 6) {
            fb.drawPixel(wx, hull_top - 26, 0x0000);
        }
    } else if (type >= 70 && type < 80) {
        fb.fillRect(x + w * 52 / 100, hull_top - 16, w * 28 / 100, 16, color);
        for (int i = 0; i < 4; ++i) {
            const int bx = x + 8 + i * (w / 6);
            fb.drawRect(bx, hull_top - 10, w / 8, 8, color);
        }
    } else if (type >= 80 && type < 90) {
        fb.fillRect(x + w * 50 / 100, hull_top - 14, w * 32 / 100, 14, color);
        for (int i = 0; i < 3; ++i) {
            const int bx = x + 12 + i * (w / 5);
            fb.drawRect(bx, hull_top - 10, w / 7, 6, color);
        }
        fb.drawLine(x + 10, hull_top - 16, x + w - 10, hull_top - 16, color);
    } else if (type == 31 || type == 32 || type == 52) {
        fb.fillRect(x + w * 55 / 100, hull_top - 30, w * 26 / 100, 30, color);
        fb.drawLine(x + w * 68 / 100, hull_top - 30, x + w * 68 / 100,
                    hull_top - 40, color);
    } else {
        fb.fillRect(x + w * 55 / 100, hull_top - 14, w * 25 / 100, 14, color);
        fb.fillRect(x + w * 35 / 100, hull_top - 8, w * 15 / 100, 8, color);
    }
}

}  // namespace

AisUIRenderer::AisUIRenderer(Framebuffer& fb) : BaseUIRenderer(fb) {
    // SONAR: deep blue-black, cyan phosphor, amber selection.
    theme_ = {0x0841, 0x10A2, 0x2124, 0x9E9F, 0x6B4D, 0x07FF,
              0x02AA, 0xF800, 0x18C3, 0x07FF};
}

void AisUIRenderer::drawChar(int x, int y, char c, uint16_t color) {
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

void AisUIRenderer::drawText(int x, int y, const char* text, uint16_t color) {
    if (!text) return;
    int cursor = x;
    while (*text) {
        drawChar(cursor, y, *text, color);
        cursor += FONT_8X16_WIDTH + 1;
        ++text;
    }
}

void AisUIRenderer::drawTextBg(int x, int y, const char* text, uint16_t fg,
                               uint16_t bg) {
    if (!text) return;
    const int width = getTextWidth(text);
    fb_.fillRect(x, y, width, FONT_8X16_HEIGHT, bg);
    drawText(x, y, text, fg);
}

int AisUIRenderer::getTextWidth(const char* text) const {
    return text ? static_cast<int>(std::strlen(text)) *
                      (FONT_8X16_WIDTH + 1)
                : 0;
}

void AisUIRenderer::drawSmallText(int x, int y, const char* text,
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

int AisUIRenderer::smallTextWidth(const char* text) const {
    return text ? static_cast<int>(std::strlen(text)) *
                      (FONT_6X8_WIDTH + 1)
                : 0;
}

void AisUIRenderer::renderStatusBar(uint64_t total_messages,
                                    float messages_per_second,
                                    int gain_tenths, bool agc,
                                    size_t vessels) {
    fb_.fillRect(0, 0, kScreenWidth, 20, theme_.status_bg);

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "AIS %zu", vessels);
    drawText(2, 2, buffer, theme_.status_text);
    if (udp_enabled_) drawText(70, 2, "UDP", 0x07E0);

    std::snprintf(buffer, sizeof(buffer), "%.1f MSG/S", messages_per_second);
    drawText(112, 2, buffer, theme_.dim);

    if (agc) {
        std::snprintf(buffer, sizeof(buffer), "AUTO");
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fDB", gain_tenths / 10.0f);
    }
    const int width = getTextWidth(buffer);
    drawText(kScreenWidth - width - 2, 2, buffer, theme_.status_text);

    fb_.fillRect(0, 19, kScreenWidth, 1, theme_.grid);
}

void AisUIRenderer::renderList(const std::vector<ais::VesselState>& vessels,
                               int selected_index, int scroll_offset,
                               uint64_t total_messages,
                               float messages_per_second, int gain_tenths,
                               bool agc) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_messages, messages_per_second, gain_tenths, agc,
                    vessels.size());

    fb_.fillRect(0, 20, kScreenWidth, 18, theme_.panel);
    drawText(2, 21, "MMSI", theme_.dim);
    drawText(84, 21, "NAME", theme_.dim);
    drawText(176, 21, "SOG", theme_.dim);
    drawText(207, 21, "COG", theme_.dim);
    drawText(238, 21, "NM", theme_.dim);
    drawText(276, 21, "AGE", theme_.dim);
    fb_.fillRect(0, 37, kScreenWidth, 1, theme_.grid);

    const int count = static_cast<int>(vessels.size());
    const int first = std::max(0, std::min(scroll_offset,
                                           std::max(0, count - kVisibleRows)));
    const time_t now = time(nullptr);

    for (int row = 0; row < kVisibleRows; ++row) {
        const int index = first + row;
        if (index >= count) break;

        const ais::VesselState& vessel = vessels[static_cast<size_t>(index)];
        const int y = 39 + row * kRowHeight;
        const bool selected = (index == selected_index);
        if (selected) {
            fb_.fillRect(0, y - 1, kScreenWidth, kRowHeight, theme_.selected);
            fb_.fillRect(0, y - 1, 2, kRowHeight, theme_.accent);
        } else if (row & 1) {
            fb_.fillRect(0, y - 1, kScreenWidth, kRowHeight, theme_.panel);
        }

        const uint16_t text_color = selected ? theme_.text : theme_.dim;

        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "%09u", vessel.mmsi);
        drawText(2, y, buffer, selected ? theme_.text : theme_.accent);

        char name[14];
        std::snprintf(name, sizeof(name), "%s%.9s", vesselMarker(vessel),
                      vessel.name.empty() ? "-----" : vessel.name.c_str());
        drawText(84, y, name, text_color);

        if (vessel.sog >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "%.1f", vessel.sog);
        } else {
            std::snprintf(buffer, sizeof(buffer), "--");
        }
        drawText(176, y, buffer, text_color);

        if (vessel.cog >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "%.0f", vessel.cog);
        } else {
            std::snprintf(buffer, sizeof(buffer), "--");
        }
        drawText(207, y, buffer, text_color);

        if (vessel.distance_nm >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "%.1f", vessel.distance_nm);
        } else {
            std::snprintf(buffer, sizeof(buffer), "--");
        }
        drawText(238, y, buffer, text_color);

        formatAge(now, vessel.last_seen, buffer, sizeof(buffer));
        drawText(276, y, buffer, vessel.stale ? theme_.dim : text_color);
    }

    renderStatsBar(total_messages, vessels.size(), gain_tenths, agc);
}

void AisUIRenderer::renderStatsBar(uint64_t total_messages, size_t vessels,
                                   int gain_tenths, bool agc) {
    fb_.fillRect(0, 152, kScreenWidth, 18, theme_.status_bg);
    fb_.fillRect(0, 152, kScreenWidth, 1, theme_.grid);

    char buffer[96];
    char gain[16];
    if (agc) {
        std::snprintf(gain, sizeof(gain), "AUTO");
    } else {
        std::snprintf(gain, sizeof(gain), "%.1fDB", gain_tenths / 10.0f);
    }
    std::snprintf(buffer, sizeof(buffer), "%llu MSG  %zu VESSELS  %s",
                  static_cast<unsigned long long>(total_messages), vessels,
                  gain);
    drawSmallText(2, 157, buffer, theme_.dim);

    const char* hint = "H HELP";
    drawSmallText(kScreenWidth - smallTextWidth(hint) - 2, 157, hint,
                  theme_.accent);
}

void AisUIRenderer::renderConsole(
    const std::vector<ais::AisLogEntry>& entries, int scroll_offset,
    uint64_t total_messages, float messages_per_second, int gain_tenths,
    bool agc, size_t vessels) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_messages, messages_per_second, gain_tenths, agc,
                    vessels);
    fb_.fillRect(0, 20, kScreenWidth, 1, theme_.grid);

    constexpr int kVisibleLines = 16;
    const int total = static_cast<int>(entries.size());
    const int end = std::max(0, total - scroll_offset);
    const int start = std::max(0, end - kVisibleLines);
    const int lines = end - start;

    if (total == 0) {
        drawText(70, 74, "WAITING FOR AIS...", theme_.accent);
    } else {
        for (int i = 0; i < lines; ++i) {
            const int index = start + i;
            const int y = 24 + i * 8;
            const bool newest = (index == total - 1);
            drawSmallText(2, y, entries[static_cast<size_t>(index)].text.c_str(),
                          newest ? theme_.accent : theme_.dim);
        }
    }

    fb_.fillRect(0, 152, kScreenWidth, 18, theme_.status_bg);
    fb_.fillRect(0, 152, kScreenWidth, 1, theme_.grid);
    drawSmallText(2, 157, "162.000 1.92M", theme_.dim);
    const char* hint = "ENTER LIST  H HELP";
    drawSmallText(kScreenWidth - smallTextWidth(hint) - 2, 157, hint,
                  theme_.accent);
}

void AisUIRenderer::renderPlot(
    const std::vector<ais::VesselState>& vessels, int selected_index,
    float range_nm, bool station_valid, double station_latitude,
    double station_longitude, uint64_t total_messages,
    float messages_per_second, int gain_tenths, bool agc,
    MapBackground* map) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);

    constexpr int cx = 160;
    constexpr int cy = 86;
    constexpr int radius = 64;
    const uint16_t selected_color = 0xFD20;
    const bool has_map = (map != nullptr && station_valid);

    if (has_map) {
        map->draw(fb_, cx, cy, radius, range_nm * 1.852f, 0.0f);
    }

    // The cached map can cover the whole display; redraw the status strip on
    // top of it.
    renderStatusBar(total_messages, messages_per_second, gain_tenths, agc,
                    vessels.size());
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "R %.0fNM", range_nm);
    drawText(200, 2, buffer, theme_.dim);

    for (int ring = 1; ring <= 4; ++ring) {
        drawCircleOutline(fb_, cx, cy, radius * ring / 4,
                          ring == 4 ? (has_map ? theme_.accent : theme_.dim)
                                    : (has_map ? theme_.dim : theme_.grid));
    }

    // 0.5 NM CPA guard ring.
    if (range_nm >= 0.5f) {
        const int guard_radius = std::max(
            1, static_cast<int>(std::lround(0.5f / range_nm * radius)));
        drawDashedCircle(fb_, cx, cy, guard_radius, selected_color);
    }

    const uint16_t grid_color = has_map ? theme_.dim : theme_.grid;
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

    // Own ship.
    fb_.drawLine(cx, cy - 5, cx - 3, cy + 3, theme_.accent);
    fb_.drawLine(cx, cy - 5, cx + 3, cy + 3, theme_.accent);
    fb_.drawLine(cx - 3, cy + 3, cx + 3, cy + 3, theme_.accent);
    fb_.drawPixel(cx, cy, theme_.text);

    int drawn = 0;

    for (int index = 0; index < static_cast<int>(vessels.size()); ++index) {
        const ais::VesselState& vessel = vessels[static_cast<size_t>(index)];
        if (!vessel.has_position || vessel.distance_nm < 0.0f) continue;
        if (vessel.distance_nm > range_nm * 1.05f) continue;

        const int x = cx + static_cast<int>(std::lround(
            std::sin(vessel.bearing_deg * kPi / 180.0) *
            (vessel.distance_nm / range_nm) * radius));
        const int y = cy - static_cast<int>(std::lround(
            std::cos(vessel.bearing_deg * kPi / 180.0) *
            (vessel.distance_nm / range_nm) * radius));

        // Historical trail, oldest first.
        for (size_t t = 0; t < vessel.trail.size(); ++t) {
            const float distance = distanceNm(
                station_latitude, station_longitude,
                vessel.trail[t].first, vessel.trail[t].second);
            if (distance > range_nm) continue;
            const float bearing = bearingDeg(
                station_latitude, station_longitude,
                vessel.trail[t].first, vessel.trail[t].second);
            const int tx = cx + static_cast<int>(std::lround(
                std::sin(bearing * kPi / 180.0) * (distance / range_nm) * radius));
            const int ty = cy - static_cast<int>(std::lround(
                std::cos(bearing * kPi / 180.0) * (distance / range_nm) * radius));
            fb_.drawPixel(tx, ty,
                          t + 2 >= vessel.trail.size() ? theme_.dim : theme_.grid);
        }

        const bool selected = (index == selected_index);
        const bool alarm = !vessel.stale && vessel.cpa_nm >= 0.0f &&
                           vessel.cpa_nm < 0.5f && vessel.tcpa_min >= 0.0f &&
                           vessel.tcpa_min < 10.0f;
        const uint16_t color = selected ? selected_color
                                        : (vessel.stale ? theme_.dim
                                                        : (alarm ? theme_.alarm
                                                                 : (vessel.class_b
                                                                        ? 0x05FF
                                                                        : theme_.accent)));

        if (vessel.cog >= 0.0f && vessel.sog > 0.3f) {
            const float length = 7.0f + std::min(13.0f, vessel.sog * 1.2f);
            const float course = vessel.cog * static_cast<float>(kPi) / 180.0f;
            const int ex = x + static_cast<int>(std::lround(std::sin(course) * length));
            const int ey = y - static_cast<int>(std::lround(std::cos(course) * length));
            fb_.drawLine(x, y, (x + ex) / 2, (y + ey) / 2, color);
            fb_.drawLine((x + ex) / 2, (y + ey) / 2, ex, ey, theme_.dim);
        }

        if (vessel.base_station) {
            fb_.drawRect(x - 2, y - 2, 5, 5, color);
        } else if (vessel.aton) {
            drawCircleOutline(fb_, x, y, 2, color);
        } else if (vessel.class_b) {
            fb_.drawLine(x, y - 4, x + 4, y, color);
            fb_.drawLine(x + 4, y, x, y + 4, color);
            fb_.drawLine(x, y + 4, x - 4, y, color);
            fb_.drawLine(x - 4, y, x, y - 4, color);
        } else {
            drawTargetTriangle(fb_, x, y,
                               vessel.cog >= 0.0f ? vessel.cog
                                                  : vessel.bearing_deg,
                               color, selected ? 5 : 4);
        }
        fb_.drawPixel(x, y, color);

        if (selected) {
            fb_.drawLine(x - 7, y, x - 3, y, selected_color);
            fb_.drawLine(x + 3, y, x + 7, y, selected_color);
            fb_.drawLine(x, y - 7, x, y - 3, selected_color);
            fb_.drawLine(x, y + 3, x, y + 7, selected_color);

            if (vessel.cpa_nm >= 0.0f && vessel.tcpa_min >= 0.0f) {
                const int px = cx + static_cast<int>(std::lround(
                    vessel.cpa_east_nm / range_nm * radius));
                const int py = cy - static_cast<int>(std::lround(
                    vessel.cpa_north_nm / range_nm * radius));
                for (int k = 0; k < 32; k += 2) {
                    const int x0 = cx + (px - cx) * k / 32;
                    const int y0 = cy + (py - cy) * k / 32;
                    const int x1 = cx + (px - cx) * (k + 1) / 32;
                    const int y1 = cy + (py - cy) * (k + 1) / 32;
                    fb_.drawLine(x0, y0, x1, y1, selected_color);
                }
                fb_.drawLine(px - 3, py - 3, px + 3, py + 3, selected_color);
                fb_.drawLine(px - 3, py + 3, px + 3, py - 3, selected_color);
            }
        }
        ++drawn;
    }

    // Selected target card on the left, translucent over the scope.
    constexpr int panel_x = 3;
    constexpr int panel_y = 24;
    constexpr int panel_w = 94;
    constexpr int panel_h = 74;
    for (int y = 0; y < panel_h; ++y) {
        for (int x = 0; x < panel_w; ++x) {
            fb_.blendPixel(panel_x + x, panel_y + y, theme_.bg, 0.78f);
        }
    }
    fb_.drawRect(panel_x, panel_y, panel_w, panel_h, theme_.grid);

    if (selected_index >= 0 &&
        selected_index < static_cast<int>(vessels.size())) {
        const ais::VesselState& vessel = vessels[static_cast<size_t>(selected_index)];
        const time_t age = time(nullptr) - vessel.last_seen;
        int line_y = panel_y + 4;
        std::snprintf(buffer, sizeof(buffer), "%09u", vessel.mmsi);
        drawSmallText(panel_x + 4, line_y, buffer, theme_.accent);
        line_y += 9;
        std::snprintf(buffer, sizeof(buffer), "%.12s",
                      vessel.name.empty() ? "-----" : vessel.name.c_str());
        drawSmallText(panel_x + 4, line_y, buffer, theme_.text);
        line_y += 9;
        const char* type = ais::AisStore::shipTypeLabel(vessel.ship_type);
        if (type[0] == '\0') {
            type = vessel.base_station ? "BASE" : vessel.aton ? "ATON"
                   : vessel.class_b ? "CLASS B" : "VESSEL";
        }
        drawSmallText(panel_x + 4, line_y, type, theme_.dim);
        line_y += 9;
        std::snprintf(buffer, sizeof(buffer), "SOG%.1f C%.0f",
                      vessel.sog >= 0.0f ? vessel.sog : 0.0f,
                      vessel.cog >= 0.0f ? vessel.cog : 0.0f);
        drawSmallText(panel_x + 4, line_y, buffer, theme_.text);
        line_y += 9;
        std::snprintf(buffer, sizeof(buffer), "RNG%.1f B%03.0f",
                      vessel.distance_nm >= 0.0f ? vessel.distance_nm : 0.0f,
                      vessel.bearing_deg);
        drawSmallText(panel_x + 4, line_y, buffer, theme_.text);
        line_y += 9;
        if (vessel.cpa_nm >= 0.0f) {
            if (vessel.tcpa_min >= 0.0f) {
                std::snprintf(buffer, sizeof(buffer), "CPA%.1f T%.0fm",
                              vessel.cpa_nm, vessel.tcpa_min);
            } else {
                std::snprintf(buffer, sizeof(buffer), "CPA%.1f T--",
                              vessel.cpa_nm);
            }
        } else {
            std::snprintf(buffer, sizeof(buffer), "CPA --");
        }
        drawSmallText(panel_x + 4, line_y, buffer, theme_.text);
        line_y += 9;
        std::snprintf(buffer, sizeof(buffer), "AGE %lds", static_cast<long>(age));
        drawSmallText(panel_x + 4, line_y, buffer, theme_.dim);
    } else {
        drawSmallText(panel_x + 4, panel_y + 4, "NO TARGET", theme_.dim);
    }

    if (!station_valid) {
        drawText(78, 120, "POSITION NOT SET", theme_.alarm);
    } else if (drawn == 0) {
        drawText(92, 74, "WAITING FOR AIS", theme_.accent);
    }

    fb_.fillRect(0, 152, kScreenWidth, 18, theme_.status_bg);
    fb_.fillRect(0, 152, kScreenWidth, 1, theme_.grid);
    std::snprintf(buffer, sizeof(buffer), "PLOT %.0fNM", range_nm);
    drawSmallText(2, 157, buffer, theme_.dim);
    drawSmallText(78, 157, "UP/DN RANGE  <> TGT", theme_.text);
    drawSmallText(kScreenWidth - smallTextWidth("H HELP") - 2, 157, "H HELP",
                  theme_.accent);
}

void AisUIRenderer::renderDossier(const ais::VesselState* vessel,
                                  size_t vessel_count,
                                  uint64_t total_messages,
                                  float messages_per_second,
                                  int gain_tenths, bool agc) {
    fb_.fillRect(0, 0, kScreenWidth, 170, theme_.bg);
    renderStatusBar(total_messages, messages_per_second, gain_tenths, agc,
                    vessel_count);

    if (vessel == nullptr) {
        drawText(104, 72, "NO TARGET", theme_.dim);
    } else {
        constexpr int panel_x = 2;
        constexpr int panel_y = 22;
        constexpr int panel_w = 104;
        constexpr int panel_h = 126;
        fb_.fillRect(panel_x, panel_y, panel_w, panel_h, theme_.panel);
        fb_.drawRect(panel_x, panel_y, panel_w, panel_h, theme_.grid);

        drawShipSilhouette(fb_, 6, 28, 96, 72, *vessel, theme_.accent);

        const int length = (vessel->to_bow >= 0 && vessel->to_stern >= 0)
                               ? vessel->to_bow + vessel->to_stern
                               : -1;
        const int beam = (vessel->to_port >= 0 && vessel->to_starboard >= 0)
                             ? vessel->to_port + vessel->to_starboard
                             : -1;
        char dim[32];
        if (length > 0) {
            std::snprintf(dim, sizeof(dim), "L %dm", length);
        } else {
            std::snprintf(dim, sizeof(dim), "L --");
        }
        drawSmallText(6, 106, dim, theme_.dim);
        if (beam > 0) {
            std::snprintf(dim, sizeof(dim), "W %dm", beam);
        } else {
            std::snprintf(dim, sizeof(dim), "W --");
        }
        drawSmallText(6, 116, dim, theme_.dim);
        if (vessel->draught >= 0.0f) {
            std::snprintf(dim, sizeof(dim), "D %.1fm", vessel->draught);
        } else {
            std::snprintf(dim, sizeof(dim), "D --");
        }
        drawSmallText(6, 126, dim, theme_.dim);

        char name[24];
        std::snprintf(name, sizeof(name), "%.18s",
                      vessel->name.empty() ? "UNKNOWN" : vessel->name.c_str());
        drawText(112, 23, name, theme_.text);

        int y = 42;
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "MMSI %09u", vessel->mmsi);
        drawSmallText(112, y, buffer, theme_.accent);
        y += 9;

        char class_line[40];
        if (vessel->base_station) {
            std::snprintf(class_line, sizeof(class_line), "BASE STATION");
        } else if (vessel->aton) {
            std::snprintf(class_line, sizeof(class_line), "AID TO NAVIGATION");
        } else {
            const char* type = ais::AisStore::shipTypeLabel(vessel->ship_type);
            if (type[0] == '\0') {
                std::snprintf(class_line, sizeof(class_line), "%s",
                              vessel->class_b ? "CLASS B" : "CLASS A");
            } else {
                std::snprintf(class_line, sizeof(class_line), "%s  %s", type,
                              vessel->class_b ? "CLASS B" : "CLASS A");
            }
        }
        drawSmallText(112, y, class_line, theme_.text);
        y += 9;

        char imo[16];
        if (vessel->imo != 0) {
            std::snprintf(imo, sizeof(imo), "%07u", vessel->imo);
        } else {
            std::snprintf(imo, sizeof(imo), "--");
        }
        std::snprintf(buffer, sizeof(buffer), "CALL %s  IMO %s",
                      vessel->callsign.empty() ? "--" : vessel->callsign.c_str(),
                      imo);
        drawSmallText(112, y, buffer, theme_.dim);
        y += 9;

        std::snprintf(buffer, sizeof(buffer), "STATUS %s",
                      navStatusLabel(vessel->nav_status));
        drawSmallText(112, y, buffer, theme_.dim);
        y += 9;

        if (vessel->has_position) {
            std::snprintf(buffer, sizeof(buffer), "POS %.5f,%.5f",
                          vessel->latitude, vessel->longitude);
        } else {
            std::snprintf(buffer, sizeof(buffer), "POS --");
        }
        drawSmallText(112, y, buffer, theme_.dim);
        y += 9;

        char sog[16], cog[16], hdg[16];
        if (vessel->sog >= 0.0f) std::snprintf(sog, sizeof(sog), "%.1f", vessel->sog);
        else std::snprintf(sog, sizeof(sog), "--");
        if (vessel->cog >= 0.0f) std::snprintf(cog, sizeof(cog), "%.0f", vessel->cog);
        else std::snprintf(cog, sizeof(cog), "--");
        if (vessel->heading >= 0) std::snprintf(hdg, sizeof(hdg), "%d", vessel->heading);
        else std::snprintf(hdg, sizeof(hdg), "--");
        std::snprintf(buffer, sizeof(buffer), "SOG %skn  COG %s  HDG %s",
                      sog, cog, hdg);
        drawSmallText(112, y, buffer, theme_.text);
        y += 9;

        if (vessel->distance_nm >= 0.0f) {
            std::snprintf(buffer, sizeof(buffer), "RNG %.1fNM  BRG %03.0f",
                          vessel->distance_nm, vessel->bearing_deg);
        } else {
            std::snprintf(buffer, sizeof(buffer), "RNG --");
        }
        drawSmallText(112, y, buffer, theme_.text);
        y += 9;

        if (vessel->cpa_nm >= 0.0f) {
            if (vessel->tcpa_min >= 0.0f) {
                std::snprintf(buffer, sizeof(buffer), "CPA %.1fNM  T%.0fm",
                              vessel->cpa_nm, vessel->tcpa_min);
            } else {
                std::snprintf(buffer, sizeof(buffer), "CPA %.1fNM  T--",
                              vessel->cpa_nm);
            }
        } else {
            std::snprintf(buffer, sizeof(buffer), "CPA --");
        }
        drawSmallText(112, y, buffer, theme_.text);
        y += 9;

        std::snprintf(buffer, sizeof(buffer), "DEST %.20s",
                      vessel->destination.empty() ? "--"
                                                  : vessel->destination.c_str());
        drawSmallText(112, y, buffer, theme_.dim);
        y += 9;

        if (vessel->eta_month > 0) {
            std::snprintf(buffer, sizeof(buffer), "ETA %02d-%02d %02d:%02d",
                          vessel->eta_month, vessel->eta_day,
                          vessel->eta_hour, vessel->eta_minute);
        } else {
            std::snprintf(buffer, sizeof(buffer), "ETA --");
        }
        drawSmallText(112, y, buffer, theme_.dim);
        y += 9;

        std::snprintf(buffer, sizeof(buffer), "AGE %lds  MSG %u",
                      static_cast<long>(time(nullptr) - vessel->last_seen),
                      vessel->messages);
        drawSmallText(112, y, buffer, theme_.dim);
    }

    fb_.fillRect(0, 152, kScreenWidth, 18, theme_.status_bg);
    fb_.fillRect(0, 152, kScreenWidth, 1, theme_.grid);
    drawSmallText(2, 157, "DOSSIER", theme_.dim);
    drawSmallText(72, 157, "<> TGT  ENTER NEXT", theme_.text);
    drawSmallText(kScreenWidth - smallTextWidth("H HELP") - 2, 157, "H HELP",
                  theme_.accent);
}

void AisUIRenderer::renderHelp() {
    fb_.fillRect(12, 28, 296, 120, theme_.panel);
    fb_.fillRect(12, 28, 296, 1, theme_.accent);
    fb_.fillRect(12, 147, 296, 1, theme_.accent);
    fb_.fillRect(12, 28, 1, 120, theme_.accent);
    fb_.fillRect(307, 28, 1, 120, theme_.accent);

    drawText(22, 34, "AIS KEYS", theme_.accent);
    const char* lines[] = {
        "ENTER  LIST / PLOT / DOSSIER / CONSOLE",
        "UP/DN  SCROLL OR RANGE",
        "LEFT/RIGHT  TARGET",
        "+/-    GAIN",
        "G      AUTO / MANUAL",
        "U      UDP NMEA 10110",
        "S      SCREENSHOT",
        "TAB    PAGES",
        "ESC    CLOSE HELP",
    };
    for (int i = 0; i < 9; ++i) {
        drawSmallText(24, 56 + i * 11, lines[i],
                      i == 0 ? theme_.text : theme_.dim);
    }
}
