#ifndef BASE_UI_RENDERER_H
#define BASE_UI_RENDERER_H

#include "framebuffer.h"
#include <cstdint>

/**
 * BaseUIRenderer - Base class providing generic UI primitives
 *
 * This class provides common UI rendering methods that can be used
 * by all pages (SDR, ADSB, Menu, etc.)
 *
 * Subclasses can override drawText/drawTextBg/getTextWidth to use
 * different fonts (e.g. ADSBUIRenderer uses an 8x16 font for readability).
 */
class BaseUIRenderer {
public:
    explicit BaseUIRenderer(Framebuffer& fb);
    virtual ~BaseUIRenderer();

    /**
     * Draw text at specified position
     * @param x X coordinate
     * @param y Y coordinate
     * @param text Text string to draw
     * @param color Text color (RGB565)
     */
    virtual void drawText(int x, int y, const char* text, uint16_t color);

    /**
     * Draw text with solid background
     * @param x X coordinate
     * @param y Y coordinate
     * @param text Text string to draw
     * @param fg Foreground (text) color
     * @param bg Background color
     */
    virtual void drawTextBg(int x, int y, const char* text, uint16_t fg, uint16_t bg);

    /**
     * Draw a filled rectangle
     * @param x X coordinate
     * @param y Y coordinate
     * @param w Width
     * @param h Height
     * @param color Fill color (RGB565)
     */
    void drawBox(int x, int y, int w, int h, uint16_t color);

    /**
     * Draw a dialog box with title, message, and hint
     * @param title Dialog title
     * @param message Main message text
     * @param hint Hint text (e.g., key bindings)
     */
    virtual void renderDialog(const char* title, const char* message, const char* hint);

    /**
     * Draw a list item
     * @param x X coordinate
     * @param y Y coordinate
     * @param text Item text
     * @param selected Whether this item is selected
     * @param width List width
     */
    void drawListItem(int x, int y, const char* text, bool selected, int width);

    /**
     * Draw a progress bar
     * @param x X coordinate
     * @param y Y coordinate
     * @param w Width
     * @param h Height
     * @param progress Progress value (0.0 to 1.0)
     * @param color Bar color
     */
    void drawProgressBar(int x, int y, int w, int h, float progress, uint16_t color);

    /**
     * Draw a horizontal line
     * @param x X coordinate
     * @param y Y coordinate
     * @param length Line length
     * @param color Line color
     */
    void drawHLine(int x, int y, int length, uint16_t color);

    /**
     * Draw a vertical line
     * @param x X coordinate
     * @param y Y coordinate
     * @param length Line length
     * @param color Line color
     */
    void drawVLine(int x, int y, int length, uint16_t color);

    /**
     * Clear the entire screen
     * @param color Background color
     */
    void clearScreen(uint16_t color);

    /**
     * Update the framebuffer (flush to display)
     */
    void updateDisplay();

protected:
    Framebuffer& fb_;

    /**
     * Get text width in pixels (for centering, etc.)
     * @param text Text string
     * @return Width in pixels
     */
    virtual int getTextWidth(const char* text) const;

    /**
     * Draw a single character using the renderer-specific font
     */
    virtual void drawChar(int x, int y, char c, uint16_t color);
};

#endif // BASE_UI_RENDERER_H
