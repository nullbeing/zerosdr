#include "core/first_run_guide.h"

#include "core/gps_locator.h"
#include "framebuffer.h"
#include "font.h"
#include "input_handler.h"
#include "adsb/adsb_config.h"
#include "ui/ttf_text.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 170;

std::string configDir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/home/pi") + "/.config/zerosdr";
}

std::string guidePath() { return configDir() + "/guide.json"; }
std::string adsbConfigPath() { return configDir() + "/adsb_config.json"; }

void ensureDir() {
    const std::string dir = configDir();
    std::string cur;
    for (char c : dir) {
        cur += c;
        if (c == '/') mkdir(cur.c_str(), 0755);
    }
    mkdir(dir.c_str(), 0755);
}

class UiText {
public:
    void draw(Framebuffer& fb, int x, int top, const char* text,
              uint16_t color, int size = 12) {
        TtfText& font = (size >= 16) ? big_ : small_;
        bool& loaded = (size >= 16) ? big_loaded_ : small_loaded_;
        if (!loaded) {
            loaded = true;
            loadDefaultUiFont(font, size >= 16 ? 16 : 12);
        }
        if (font.valid()) {
            font.draw(fb, x, top + size - 2, text, color);
            return;
        }
        // Bitmap fallback: 8x16.
        int cursor = x;
        for (const char* p = text; *p; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20 || c > 0x7E) c = '?';
            const uint8_t* glyph = font8x16[c - 0x20];
            for (int row = 0; row < FONT_8X16_HEIGHT; ++row) {
                const uint8_t line = glyph[row];
                for (int col = 0; col < FONT_8X16_WIDTH; ++col) {
                    if (line & (0x80 >> col)) {
                        fb.drawPixel(cursor + col, top + row, color);
                    }
                }
            }
            cursor += FONT_8X16_WIDTH + 1;
        }
    }

    int width(const char* text, int size = 12) {
        TtfText& font = (size >= 16) ? big_ : small_;
        bool& loaded = (size >= 16) ? big_loaded_ : small_loaded_;
        if (!loaded) {
            loaded = true;
            loadDefaultUiFont(font, size >= 16 ? 16 : 12);
        }
        if (font.valid()) return font.textWidth(text);
        return static_cast<int>(strlen(text)) * (FONT_8X16_WIDTH + 1);
    }

private:
    TtfText small_;
    TtfText big_;
    bool small_loaded_ = false;
    bool big_loaded_ = false;
};

}  // namespace

bool guideShouldShow() {
    std::ifstream file(guidePath());
    if (!file.is_open()) return true;
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    if (text.find("\"show\"") == std::string::npos) return true;
    return text.find("false") == std::string::npos;
}

void setGuideShow(bool show) {
    ensureDir();
    std::ofstream file(guidePath());
    if (!file.is_open()) return;
    file << "{\n  \"show\": " << (show ? "true" : "false") << "\n}\n";
}

void runFirstRunGuide(Framebuffer& fb, InputHandler& input) {
    ensureDir();

    ADSBConfig config;
    config.load(adsbConfigPath());

    GpsLocator gps;
    gps.poll();

    UiText text;
    int page = 0;  // 0 = keys, 1 = position, 2 = finish
    bool editing = false;
    int field = 0;  // 0 = lat, 1 = lon
    bool show_again = false;
    bool manual_entered = false;

    char lat_buf[24] = {0};
    char lon_buf[24] = {0};
    if (config.isObserverPositionSet()) {
        snprintf(lat_buf, sizeof(lat_buf), "%.4f", config.getObserverLat());
        snprintf(lon_buf, sizeof(lon_buf), "%.4f", config.getObserverLon());
    }

    auto draw = [&]() {
        fb.clear(COLOR_BG_DARK);

        if (page == 0) {
            text.draw(fb, 12, 10, "WELCOME TO zeroSDR", COLOR_YELLOW, 16);
            int y = 40;
            const char* rows[][2] = {
                {"H", "key list + settings"},
                {"TAB", "SDR <-> ADSB"},
                {"ENTER", "ADSB: list > detail > radar"},
                {"< >", "change guide page"},
            };
            for (auto& r : rows) {
                text.draw(fb, 16, y, r[0], COLOR_YELLOW, 12);
                text.draw(fb, 90, y, r[1], COLOR_TEXT_BRIGHT, 12);
                y += 20;
            }
            text.draw(fb, 12, 140, "ENTER: next page    ESC: skip", COLOR_TEXT_DIM, 12);
        } else if (page == 1) {
            text.draw(fb, 12, 10, "POSITION", COLOR_YELLOW, 16);

            char gps_line[64];
            if (gps.hasFix()) {
                snprintf(gps_line, sizeof(gps_line), "GPS: %.4f, %.4f (%s)",
                         gps.latitude(), gps.longitude(), gps.sourceName());
            } else {
                snprintf(gps_line, sizeof(gps_line), "GPS: %s", gps.statusText());
            }
            text.draw(fb, 12, 34, gps_line, COLOR_TEXT_BRIGHT, 12);

            char lat_line[48];
            char lon_line[48];
            snprintf(lat_line, sizeof(lat_line), "LAT  %s%s",
                     lat_buf[0] ? lat_buf : "----",
                     (editing && field == 0) ? "_" : "");
            snprintf(lon_line, sizeof(lon_line), "LON  %s%s",
                     lon_buf[0] ? lon_buf : "----",
                     (editing && field == 1) ? "_" : "");
            uint16_t lat_color =
                (!editing && field == 0) || (editing && field == 0)
                    ? COLOR_YELLOW
                    : COLOR_TEXT_BRIGHT;
            uint16_t lon_color =
                (!editing && field == 1) || (editing && field == 1)
                    ? COLOR_YELLOW
                    : COLOR_TEXT_BRIGHT;
            text.draw(fb, 20, 58, lat_line, lat_color, 12);
            text.draw(fb, 20, 78, lon_line, lon_color, 12);

            text.draw(fb, 12, 104, "UP/DOWN: select   ENTER: edit/confirm",
                      COLOR_TEXT_DIM, 12);
            text.draw(fb, 12, 120, "digits and . to type, BACKSPACE deletes",
                      COLOR_TEXT_DIM, 12);
            text.draw(fb, 12, 136,
                      "GPS updates the position unless you edit",
                      COLOR_TEXT_DIM, 12);
            text.draw(fb, 12, 152, "LEFT/RIGHT: page    ESC: skip",
                      COLOR_TEXT_DIM, 12);
        } else {
            text.draw(fb, 12, 10, "READY", COLOR_YELLOW, 16);
            text.draw(fb, 12, 44, "You can reopen this guide from",
                      COLOR_TEXT_BRIGHT, 12);
            text.draw(fb, 12, 62, "H > settings > SHOW GUIDE.", COLOR_TEXT_BRIGHT, 12);

            text.draw(fb, 16, 96, show_again ? "[x]" : "[ ]", COLOR_YELLOW, 12);
            text.draw(fb, 60, 96, "Show this guide on next start",
                      COLOR_TEXT_BRIGHT, 12);
            text.draw(fb, 12, 120, "SPACE: toggle", COLOR_TEXT_DIM, 12);
            text.draw(fb, 12, 144, "ENTER: start zeroSDR", COLOR_YELLOW, 12);
        }

        fb.update();
    };

    auto finish = [&]() {
        if (manual_entered) {
            config.setObserverPosition(atof(lat_buf), atof(lon_buf));
            config.setManualOverride(true);
            config.save(adsbConfigPath());
        }
        setGuideShow(show_again);
        fb.clear(COLOR_BG_DARK);
        fb.update();
    };

    draw();
    bool running = true;
    while (running) {
        usleep(30000);

        if (page == 1) gps.poll();

        const AppKeyCode key = input.pollKey();
        if (key == APPKEY_NONE) continue;

        if (editing) {
            if (key >= APPKEY_0 && key <= APPKEY_9) {
                char* buf = (field == 0) ? lat_buf : lon_buf;
                const size_t len = strlen(buf);
                if (len + 2 < sizeof(lat_buf)) {
                    buf[len] = static_cast<char>('0' + (key - APPKEY_0));
                    buf[len + 1] = '\0';
                }
            } else if (key == APPKEY_DOT) {
                char* buf = (field == 0) ? lat_buf : lon_buf;
                const size_t len = strlen(buf);
                if (strchr(buf, '.') == nullptr && len + 2 < sizeof(lat_buf)) {
                    if (len == 0) {
                        buf[0] = '0';
                        buf[1] = '.';
                        buf[2] = '\0';
                    } else {
                        buf[len] = '.';
                        buf[len + 1] = '\0';
                    }
                }
            } else if (key == APPKEY_BACKSPACE) {
                char* buf = (field == 0) ? lat_buf : lon_buf;
                const size_t len = strlen(buf);
                if (len > 0) buf[len - 1] = '\0';
            } else if (key == APPKEY_ENTER) {
                editing = false;
                if (field == 0) {
                    field = 1;
                } else {
                    const double lat = atof(lat_buf);
                    const double lon = atof(lon_buf);
                    if (lat >= -90.0 && lat <= 90.0 &&
                        lon >= -180.0 && lon <= 180.0 &&
                        lat_buf[0] && lon_buf[0]) {
                        manual_entered = true;
                    }
                }
            }
            draw();
            continue;
        }

        if (key == APPKEY_ESC) {
            running = false;
        } else if (key == APPKEY_LEFT) {
            page = (page + 2) % 3;
            editing = false;
        } else if (key == APPKEY_RIGHT) {
            page = (page + 1) % 3;
            editing = false;
        } else if (key == APPKEY_UP) {
            if (page == 1) field = 0;
        } else if (key == APPKEY_DOWN) {
            if (page == 1) field = 1;
        } else if (key == APPKEY_SPACE) {
            if (page == 2) show_again = !show_again;
        } else if (key == APPKEY_ENTER) {
            if (page == 0) {
                page = 1;
            } else if (page == 1) {
                if (field == 0) {
                    editing = true;
                    lat_buf[0] = '\0';
                } else {
                    editing = true;
                    lon_buf[0] = '\0';
                }
            } else {
                running = false;
            }
        }
        draw();
    }

    finish();
}
