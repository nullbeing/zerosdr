#include "ui/ais_ui_renderer.h"

#include "font.h"

#include <algorithm>
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

void AisUIRenderer::renderHelp() {
    fb_.fillRect(12, 28, 296, 120, theme_.panel);
    fb_.fillRect(12, 28, 296, 1, theme_.accent);
    fb_.fillRect(12, 147, 296, 1, theme_.accent);
    fb_.fillRect(12, 28, 1, 120, theme_.accent);
    fb_.fillRect(307, 28, 1, 120, theme_.accent);

    drawText(22, 34, "AIS KEYS", theme_.accent);
    const char* lines[] = {
        "ENTER  LIST / CONSOLE",
        "UP/DN  SCROLL",
        "+/-    GAIN",
        "G      AUTO / MANUAL",
        "S      SCREENSHOT",
        "TAB    PAGES",
        "ESC    CLOSE HELP",
    };
    for (int i = 0; i < 7; ++i) {
        drawSmallText(24, 58 + i * 12, lines[i],
                      i == 0 ? theme_.text : theme_.dim);
    }
}
