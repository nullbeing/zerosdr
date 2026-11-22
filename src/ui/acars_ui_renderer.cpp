#include "ui/acars_ui_renderer.h"

#include "font.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

std::string clockString(double timestamp) {
    time_t seconds = static_cast<time_t>(timestamp);
    if (seconds <= 0) seconds = time(nullptr);
    struct tm local {};
    localtime_r(&seconds, &local);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", local.tm_hour,
                  local.tm_min, local.tm_sec);
    return buffer;
}

std::vector<std::string> wrapText(const std::string& text, int columns) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
        if (static_cast<int>(current.size()) >= columns) {
            size_t cut = current.find_last_of(' ');
            if (cut != std::string::npos && cut > 10) {
                lines.push_back(current.substr(0, cut));
                current.erase(0, cut + 1);
            } else {
                lines.push_back(current);
                current.clear();
            }
        }
    }
    if (!current.empty()) lines.push_back(current);
    if (lines.empty()) lines.push_back("");
    return lines;
}

std::string firstLine(const std::string& text) {
    const size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

}  // namespace

AcarsUIRenderer::AcarsUIRenderer(Framebuffer& fb) : fb_(fb) {}

void AcarsUIRenderer::drawChar(int x, int y, char c, uint16_t color) {
    if (c < 0x20 || c > 0x7e) return;
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

void AcarsUIRenderer::drawText(int x, int y, const char* text,
                               uint16_t color) {
    if (!text) return;
    int cursor = x;
    while (*text) {
        drawChar(cursor, y, *text, color);
        cursor += FONT_8X16_WIDTH + 1;
        ++text;
    }
}

void AcarsUIRenderer::drawSmall(int x, int y, const char* text,
                                uint16_t color) {
    if (!text) return;
    int cursor = x;
    while (*text) {
        const char c = *text;
        if (c >= 0x20 && c <= 0x7e) {
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

int AcarsUIRenderer::smallWidth(const char* text) const {
    return text ? static_cast<int>(std::strlen(text)) *
                      (FONT_6X8_WIDTH + 1)
                : 0;
}

void AcarsUIRenderer::drawPanel(int x, int y, int w, int h, uint16_t color) {
    fb_.fillRect(x, y, w, h, color);
    fb_.drawRect(x, y, w, h, 0x39E7);
}

void AcarsUIRenderer::renderList(
    const std::vector<acars::Message>& messages, int selected_index,
    uint64_t total, float rate, const std::vector<float>& channels,
    float gain_db, bool running, const std::string& error) {
    fb_.fillRect(0, 0, 320, 170, kBg);

    drawText(2, 2, "ACARS", kAccent);
    char status[48];
    std::snprintf(status, sizeof(status), "%llu MSG  %.1f/M",
                  static_cast<unsigned long long>(total), rate * 60.0f);
    drawSmall(320 - smallWidth(status) - 2, 6, status, kDim);

    drawSmall(2, 21, "TIME", kDim);
    drawSmall(62, 21, "FLIGHT", kDim);
    drawSmall(118, 21, "TAIL", kDim);
    drawSmall(169, 21, "LBL", kDim);
    drawSmall(192, 21, "TEXT", kDim);
    fb_.fillRect(0, 30, 320, 1, kGrid);

    if (messages.empty()) {
        const char* waiting = running ? "WAITING FOR ACARS" : "ACARS NOT RUNNING";
        drawSmall((320 - smallWidth(waiting)) / 2, 74, waiting,
                  running ? kAccent : kRed);
        if (!error.empty()) {
            drawSmall((320 - smallWidth(error.c_str())) / 2, 88,
                      error.c_str(), kRed);
        }
        std::string channel_line = "CH";
        for (float channel : channels) {
            char part[24];
            std::snprintf(part, sizeof(part), "  %.3f", channel);
            channel_line += part;
        }
        drawSmall((320 - smallWidth(channel_line.c_str())) / 2, 104,
                  channel_line.c_str(), kDim);
        drawSmall((320 - smallWidth("R RESTART   TAB PAGES")) / 2, 124,
                  "R RESTART   TAB PAGES", kDim);
    } else {
        const int count = static_cast<int>(messages.size());
        const int selected = std::max(0, std::min(selected_index, count - 1));
        int first = 0;
        constexpr int kRows = 9;
        if (selected >= kRows) first = selected - kRows + 1;

        for (int row = 0; row < kRows; ++row) {
            const int index = first + row;
            if (index >= count) break;
            const acars::Message& msg = messages[static_cast<size_t>(index)];
            const int y = 33 + row * 13;
            const bool is_selected = index == selected;
            if (is_selected) {
                fb_.fillRect(0, y - 1, 320, 12, kPanel);
                fb_.fillRect(0, y - 1, 2, 12, kAccent);
            }

            const uint16_t color = is_selected ? 0xFFFF : kText;
            const std::string time = clockString(msg.timestamp);
            drawSmall(2, y, time.c_str(), kDim);

            std::string flight = msg.flight.empty() ? "--" : msg.flight;
            if (flight.size() > 8) flight.resize(8);
            drawSmall(62, y, flight.c_str(), is_selected ? 0xFFFF : kAccent);

            std::string tail = msg.tail.empty() ? "--" : msg.tail;
            if (tail.size() > 7) tail.resize(7);
            drawSmall(118, y, tail.c_str(), color);

            std::string label = msg.label.empty() ? "--" : msg.label;
            if (label.size() > 3) label.resize(3);
            drawSmall(169, y, label.c_str(), kText);

            std::string snippet = firstLine(msg.text);
            if (snippet.size() > 18) snippet.resize(18);
            drawSmall(192, y, snippet.c_str(), color);
        }
    }

    char footer[64];
    float channel = channels.empty() ? 0.0f : channels.front();
    if (channels.size() > 1) {
        std::snprintf(footer, sizeof(footer), "%.3f +%zu  %.0fDB",
                      channel, channels.size() - 1, gain_db);
    } else {
        std::snprintf(footer, sizeof(footer), "%.3f  %.0fDB", channel,
                      gain_db);
    }
    drawSmall(2, 158, footer, kDim);
    const char* hint = messages.empty() ? "H HELP" : "ENT DETAIL  H HELP";
    drawSmall(320 - smallWidth(hint) - 2, 158, hint, kDim);
}

void AcarsUIRenderer::renderDetail(const acars::Message& message,
                                   int scroll) {
    fb_.fillRect(0, 0, 320, 170, kBg);

    std::string title = message.flight;
    if (title.empty()) title = message.tail.empty() ? "ACARS" : message.tail;
    drawText(2, 2, title.c_str(), kAccent);

    std::string label = message.label.empty() ? "--" : message.label;
    char right[32];
    std::snprintf(right, sizeof(right), "%s  %.3f", label.c_str(),
                  message.freq_mhz);
    drawSmall(320 - smallWidth(right) - 2, 6, right, kText);

    char meta[64];
    std::snprintf(meta, sizeof(meta), "%s  %s  L:%d",
                  clockString(message.timestamp).c_str(),
                  message.tail.empty() ? "--" : message.tail.c_str(),
                  static_cast<int>(message.level));
    drawSmall(2, 21, meta, kDim);
    char meta2[48];
    std::snprintf(meta2, sizeof(meta2), "MODE %s  ERR %d%s",
                  message.mode.empty() ? "-" : message.mode.c_str(),
                  message.error, message.ack ? "  ACK" : "");
    drawSmall(320 - smallWidth(meta2) - 2, 21, meta2, kDim);
    fb_.fillRect(0, 31, 320, 1, kGrid);

    const auto lines = wrapText(message.text, 45);
    constexpr int kVisible = 12;
    const int max_scroll =
        std::max(0, static_cast<int>(lines.size()) - kVisible);
    scroll = std::max(0, std::min(scroll, max_scroll));

    for (int i = 0; i < kVisible; ++i) {
        const int index = scroll + i;
        if (index >= static_cast<int>(lines.size())) break;
        drawSmall(2, 35 + i * 10, lines[static_cast<size_t>(index)].c_str(),
                  kText);
    }

    if (max_scroll > 0) {
        char scroll_text[24];
        std::snprintf(scroll_text, sizeof(scroll_text), "%d/%d", scroll + 1,
                      max_scroll + 1);
        drawSmall(320 - smallWidth(scroll_text) - 2, 148, scroll_text, kDim);
    }
    drawSmall(2, 160, "UP/DN SCROLL   ESC BACK   H HELP", kDim);
}

void AcarsUIRenderer::renderHelp() {
    drawPanel(12, 26, 296, 120, kPanel);
    fb_.fillRect(12, 26, 296, 1, kAccent);
    drawText(22, 32, "ACARS KEYS", kAccent);
    const char* lines[] = {
        "ENTER   OPEN MESSAGE",
        "UP/DN   SELECT OR SCROLL",
        "R       RESTART DECODER",
        "TAB     PAGE MENU",
        "ESC     BACK / CLOSE",
        "",
        "CHANNELS ARE SET IN config.json",
        "ACARS CHANNELS DEPEND ON REGION",
    };
    for (int i = 0; i < 8; ++i) {
        drawSmall(24, 54 + i * 10, lines[i], i < 5 ? kText : kDim);
    }
}

void AcarsUIRenderer::renderDialog(const char* title, const char* message,
                                   const char* hint) {
    fb_.fillRect(20, 56, 280, 60, kPanel);
    fb_.drawRect(20, 56, 280, 60, 0xFFFF);
    fb_.drawRect(21, 57, 278, 58, 0xFFFF);
    const int title_width = static_cast<int>(std::strlen(title)) * 8;
    drawText((320 - title_width) / 2, 64, title, kAccent);
    const int message_width = static_cast<int>(std::strlen(message)) * 7;
    drawSmall((320 - message_width) / 2, 88, message, kText);
    const int hint_width = static_cast<int>(std::strlen(hint)) * 7;
    drawSmall((320 - hint_width) / 2, 102, hint, kDim);
}
