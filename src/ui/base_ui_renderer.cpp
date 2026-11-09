#include "ui/base_ui_renderer.h"
#include "font.h"
#include <cstring>
#include <algorithm>

BaseUIRenderer::BaseUIRenderer(Framebuffer& fb) : fb_(fb) {
}

BaseUIRenderer::~BaseUIRenderer() {
}

void BaseUIRenderer::drawChar(int x, int y, char c, uint16_t color) {
    // Only support printable ASCII
    if (c < 0x20 || c > 0x7E) return;

    // Default 6x8 font
    const uint8_t* glyph = font6x8[c - 0x20];

    for (int row = 0; row < FONT_6X8_HEIGHT; row++) {
        uint8_t line = glyph[row];
        for (int col = 0; col < FONT_6X8_WIDTH; col++) {
            if (line & (0x80 >> col)) {
                fb_.drawPixel(x + col, y + row, color);
            }
        }
    }
}

void BaseUIRenderer::drawText(int x, int y, const char* text, uint16_t color) {
    if (!text) return;

    int cursor_x = x;
    while (*text) {
        drawChar(cursor_x, y, *text, color);
        cursor_x += FONT_6X8_WIDTH + 1; // character width + 1 pixel spacing
        text++;
    }
}

void BaseUIRenderer::drawTextBg(int x, int y, const char* text, uint16_t fg, uint16_t bg) {
    if (!text) return;

    int text_len = 0;
    const char* p = text;
    while (*p++) text_len++;

    fb_.fillRect(x, y, text_len * (FONT_6X8_WIDTH + 1), FONT_6X8_HEIGHT, bg);
    drawText(x, y, text, fg);
}

void BaseUIRenderer::drawBox(int x, int y, int w, int h, uint16_t color) {
    fb_.fillRect(x, y, w, h, color);
}

void BaseUIRenderer::renderDialog(const char* title, const char* message, const char* hint) {
    const int dialog_w = 280;
    const int dialog_h = 80;
    const int dialog_x = (320 - dialog_w) / 2;
    const int dialog_y = 90;  // Position in waterfall area

    //g background
    fb_.fillRect(dialog_x, dialog_y, dialog_w, dialog_h, COLOR_DIALOG_BG);

    // Draw border
    for (int i = 0; i < 2; i++) {
        // Top and bottom
        for (int dx = 0; dx < dialog_w; dx++) {
            fb_.drawPixel(dialog_x + dx, dialog_y + i, COLOR_DIALOG_BORDER);
            fb_.drawPixel(dialog_x + dx, dialog_y + dialog_h - 1 - i, COLOR_DIALOG_BORDER);
        }
        // Left and right
        for (int dy = 0; dy < dialog_h; dy++) {
            fb_.drawPixel(dialog_x + i, dialog_y + dy, COLOR_DIALOG_BORDER);
            fb_.drawPixel(dialog_x + dialog_w - 1 - i, dialog_y + dy, COLOR_DIALOG_BORDER);
        }
    }

    // Draw title (centered)
    int title_width = getTextWidth(title);
    int title_x = dialog_x + (dialog_w - title_width) / 2;
    drawText(title_x, dialog_y + 10, title, COLOR_TEXT_BRIGHT);

    // Draw message (centered)
    int msg_width = getTextWidth(message);
    int msg_x = dialog_x + (dialog_w - msg_width) / 2;
    drawText(msg_x, dialog_y + 30, message, COLOR_TEXT_DIM);

    // Draw hint (centered)
    if (hint) {
        int hint_width = getTextWidth(hint);
        int hint_x = dialog_x + (dialog_w - hint_width) / 2;
        drawText(hint_x, dialog_y + 55, hint, COLOR_TEXT_BRIGHT);
    }
}

void BaseUIRenderer::drawListItem(int x, int y, const char* text, bool selected, int width) {
    // Draw background
    uint16_t bg_color = selected ? COLOR_SELECTED : COLOR_BG_DARK;
    fb_.fillRect(x, y, width, 12, bg_color);

    // Draw text
    uint16_t text_color = selected ? COLOR_TEXT_BRIGHT : COLOR_TEXT_DIM;
    drawText(x + 4, y + 2, text, text_color);

    // Draw selection indicator
    if (selected) {
        drawText(x + width - 10, y + 2, ">", COLOR_TEXT_BRIGHT);
    }
}

void BaseUIRenderer::drawProgressBar(int x, int y, int w, int h, float progress, uint16_t color) {
    // Clamp progress to [0, 1]
    progress = std::max(0.0f, std::min(1.0f, progress));

    // Draw border
    fb_.fillRect(x, y, w, h, COLOR_GRID);

    // Draw filled portion
    int fill_width = (int)((w - 4) * progress);
    if (fill_width > 0) {
        fb_.fillRect(x + 2, y + 2, fill_width, h - 4, color);
    }
}

void BaseUIRenderer::drawHLine(int x, int y, int length, uint16_t color) {
    for (int i = 0; i < length; i++) {
        fb_.drawPixel(x + i, y, color);
    }
}

void BaseUIRenderer::drawVLine(int x, int y, int length, uint16_t color) {
    for (int i = 0; i < length; i++) {
        fb_.drawPixel(x, y + i, color);
    }
}

void BaseUIRenderer::clearScreen(uint16_t color) {
    fb_.clear(color);
}

void BaseUIRenderer::updateDisplay() {
    fb_.update();
}

int BaseUIRenderer::getTextWidth(const char* text) const {
    if (!text) return 0;
    // 6x8 font: 6 pixels per char + 1 pixel spacing between chars
    return strlen(text) * (FONT_6X8_WIDTH + 1);
}
