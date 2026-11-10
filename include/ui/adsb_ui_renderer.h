#ifndef ADSB_UI_RENDERER_H
#define ADSB_UI_RENDERER_H

#include "ui/base_ui_renderer.h"
#include "ui/ttf_text.h"
#include "adsb/adsb_message.h"
#include <vector>

/**
 * ADSBUIRenderer - UI renderer for ADSB page
 *
 * Renders aircraft list in table format with fixed-width columns
 * Layout optimized for 320x170 display
 */
class ADSBUIRenderer : public BaseUIRenderer {
public:
    explicit ADSBUIRenderer(Framebuffer& fb);
    ~ADSBUIRenderer();

    /**
     * Render aircraft list in table format
     * @param aircraft Vector of aircraft to display
     * @param selected_index Index of selected aircraft (-1 for none)
     * @param scroll_offset Scroll offset for list
     */
    void renderAircraftList(const std::vector<AircraftState>& aircraft,
                           int selected_index,
                           int scroll_offset);

    /**
     * Render top status bar
     * @param aircraft_count Number of aircraft
     * @param message_count Total messages decoded
     * @param message_rate Messages per second
     * @param gain Current gain value
     */
    void renderStatusBar(int aircraft_count,
                        uint32_t message_count,
                        float message_rate);

    /**
     * Render bottom statistics bar
     * @param total_messages Total messages
     * @param crc_errors CRC error count
     * @param max_range Maximum range (km)
     */
    void renderStatistics(uint32_t total_messages,
                         uint32_t crc_errors,
                         float max_range);

    /**
     * Render column headers
     */
    void renderColumnHeaders();

    // H overlay: key reference and settings summary
    void renderKeyHelp();
    void renderSettingsOverview(double lat, double lon, bool pos_set, int selected,
                                const char* source);

    /**
     * Render detail view for selected aircraft
     * @param aircraft Aircraft to display
     * @param scroll_offset Vertical scroll offset in pixels
     * @param aircraft_count Total aircraft count for status bar
     * @param message_count Total messages for status bar
     * @param message_rate Messages per second for status bar
     * @param gain Current gain value for status bar
     */
    void renderAircraftDetail(const AircraftState& aircraft,
                              int scroll_offset,
                              int aircraft_count,
                              uint32_t message_count,
                              float message_rate,
                              float gain);

    /**
     * Render text at specified position
     * @param x X coordinate
     * @param y Y coordinate
     * @param text Text to render
     * @param color Text color
     * @param bold Use bold font (ignored, for compatibility)
     */
    void renderText(int x, int y, const char* text, uint16_t color, bool bold = false);

    // Use 8x16 font for the ADSB page (override BaseUIRenderer defaults)
    void drawChar(int x, int y, char c, uint16_t color) override;
    void drawText(int x, int y, const char* text, uint16_t color) override;
    void drawTextBg(int x, int y, const char* text, uint16_t fg, uint16_t bg) override;
    int getTextWidth(const char* text) const override;

private:
    // Anti-aliased font for status/detail text (loaded lazily).
    mutable TtfText ui_font_;
    mutable bool ui_font_loaded_ = false;
    void ensureUiFont() const;

    // Layout constants (320x170 display) - adjusted for 8x16 font
    static const int STATUS_BAR_Y = 0;
    static const int STATUS_BAR_H = 20;      // 16px font + 4px padding
    static const int HEADER_Y = 20;
    static const int HEADER_H = 18;          // 16px font + 2px padding
    static const int LIST_Y = 38;
    static const int LIST_H = 114;           // 38 to 152
    static const int STATS_BAR_Y = 152;
    static const int STATS_BAR_H = 18;       // 16px font + 2px padding
    static const int ROW_HEIGHT = 19;        // 16px font + 3px padding
    static const int MAX_VISIBLE_ROWS = 6;   // 114 / 19 = 6

    // Column positions and widths (8 pixels per character + 1px spacing)
    static const int COL_ICAO_X = 2;
    static const int COL_ICAO_W = 54;        // 6 chars * 9px
    static const int COL_CALL_X = 60;
    static const int COL_CALL_W = 72;        // 8 chars * 9px (icon + 7 chars)
    static const int COL_ALT_X = 136;
    static const int COL_ALT_W = 45;         // 5 chars * 9px
    static const int COL_SPD_X = 185;
    static const int COL_SPD_W = 27;         // 3 chars * 9px
    static const int COL_HDG_X = 216;
    static const int COL_HDG_W = 27;         // 3 chars * 9px
    static const int COL_DIST_X = 247;
    static const int COL_DIST_W = 45;        // 5 chars * 9px
    static const int COL_SEEN_X = 296;
    static const int COL_SEEN_W = 22;        // 4 chars * 9px (reduced to fit)

    /**
     * Format time since last seen
     * @param seconds Seconds since last message
     * @param buffer Output buffer (must be at least 5 chars)
     */
    void formatTimeSince(time_t seconds, char* buffer);

    /**
     * Format distance with unit
     * @param distance Distance in km
     * @param buffer Output buffer (must be at least 6 chars)
     */
    void formatDistance(float distance, char* buffer);

    // PFD rendering helpers
    void renderPFD(const AircraftState& aircraft, int x, int y, int w, int h);
    void drawArtificialHorizon(int cx, int cy, int radius, float pitch_angle);
    void drawPitchLadder(int cx, int cy, int width, int height, float current_pitch);
    void drawBankAngleIndicator(int cx, int cy, int radius);
    void drawAltitudeTape(int x, int y, int w, int h, int altitude);
    void drawSpeedTape(int x, int y, int w, int h, int speed);
    void drawHeadingIndicator(int x, int y, int w, int h, int heading);
    void drawVerticalSpeedIndicator(int x, int y, int w, int h, int vs);
    void drawCircleApprox(int cx, int cy, int radius, uint16_t color);
    float calculatePitchAngle(const AircraftState& aircraft);

    // 6x8 small font drawing (for labels on PFD instruments)
    void drawSmallText(int x, int y, const char* text, uint16_t color);
};

#endif // ADSB_UI_RENDERER_H
