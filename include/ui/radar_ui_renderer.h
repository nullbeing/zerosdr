#ifndef RADAR_UI_RENDERER_H
#define RADAR_UI_RENDERER_H

#include "ui/base_ui_renderer.h"
#include "ui/ttf_text.h"
#include "adsb/adsb_message.h"
#include <vector>
#include <cmath>

class MapBackground;

/**
 * RadarUIRenderer - UI renderer for radar page
 *
 * Renders professional radar display with:
 * - Circular radar scope with range rings
 * - Rotating sweep line with fading trail
 * - Aircraft blips in polar coordinates
 * - Compass markings
 * Layout optimized for 320x170 display
 */
struct RadarTheme {
    uint16_t bg;
    uint16_t grid;
    uint16_t sweep;
    uint16_t blip;
    uint16_t selected;
    uint16_t text;
    uint16_t text_selected;
    uint16_t list_bg;
    uint16_t list_selected;
    uint16_t panel_strip;
    uint16_t status_bg;
    uint16_t status_text;
};

class RadarUIRenderer : public BaseUIRenderer {
public:
    explicit RadarUIRenderer(Framebuffer& fb);
    ~RadarUIRenderer();

    /** 0 = original green scope, 1 = red tech scope. */
    void setTheme(int index);
    uint16_t backgroundColor() const { return theme_.bg; }
    uint16_t textColor() const { return theme_.text; }

    /**
     * Render complete radar display with left-right split layout
     * @param aircraft Vector of aircraft to display
     * @param sweep_angle Current sweep angle (0-360°)
     * @param max_range Maximum range in km
     * @param observer_lat Observer latitude
     * @param observer_lon Observer longitude
     * @param selected_index Index of selected aircraft (-1 for none)
     * @param scroll_offset Scroll offset for aircraft list
     * @param blink_state Blink state for selected aircraft (true = visible)
     * @param heading_deg Heading placed at the top of the scope (0 = north up)
     * @param zoom Scope scale (1.0 = the normal right-hand panel)
     * @param clear_screen Clear the whole display first (false when the
     *                     status-bar background must stay underneath)
     */
    void renderRadarDisplay(const std::vector<AircraftState>& aircraft,
                           float sweep_angle,
                           float max_range,
                           double observer_lat,
                           double observer_lon,
                           int selected_index,
                           int scroll_offset,
                           bool blink_state,
                           float heading_deg = 0.0f,
                           float zoom = 1.0f,
                           bool clear_screen = true);

    /**
     * Draw the full-screen map underlay. Call before the status background
     * and renderRadarDisplay so the panels, rings and text stay on top.
     */
    void drawMapBackground(MapBackground& map, float max_range, float zoom,
                           float heading_deg);

    /**
     * Render top status bar
     * @param aircraft_count Number of aircraft
     * @param max_range Maximum range (km)
     * @param heading_valid Whether the compass has a heading
     * @param heading_deg Compass heading in degrees
     * @param heading_up Whether the scope is heading-up (otherwise north-up)
     * @param zoom Current scope scale (shown as a multiplier)
     */
    void renderStatusBar(int aircraft_count, float max_range,
                         bool heading_valid = false,
                         float heading_deg = 0.0f,
                         bool heading_up = false,
                         float zoom = 1.0f);

    /** Status-bar background only. Used before the scope so the radar
     *  circle can pass above the bar; the text follows afterwards. */
    void renderStatusBackground(float zoom = 1.0f);

    /** Status-bar text only, drawn on top of the enlarged radar scope. */
    void renderStatusText(int aircraft_count, float max_range,
                          bool heading_valid = false,
                          float heading_deg = 0.0f,
                          bool heading_up = false,
                          float zoom = 1.0f);

    /**
     * Render aircraft list in left panel
     * @param aircraft Vector of aircraft to display
     * @param selected_index Index of selected aircraft
     * @param scroll_offset Scroll offset for list
     * @param background_alpha Opacity of the panel over an enlarged scope
     */
    void renderAircraftList(const std::vector<AircraftState>& aircraft,
                           int selected_index,
                           int scroll_offset,
                           float background_alpha = 1.0f);

    /**
     * Render text at specified position
     * @param x X coordinate
     * @param y Y coordinate
     * @param text Text to render
     * @param color Text color
     * @param bold Use bold font (ignored, for compatibility)
     */
    void renderText(int x, int y, const char* text, uint16_t color, bool bold = false);

private:
    RadarTheme theme_{};

    // Layout constants (320x170 display) - Left-right split layout
    static const int STATUS_BAR_Y = 0;
    static const int STATUS_BAR_H = 16;

    // Left panel - Aircraft list
    static const int LIST_X = 0;
    static const int LIST_Y = 16;
    static const int LIST_W = 126;  // Left 118 pixels for list
    static const int LIST_H = 154;  // 170 - 16 = 154
    static const int ROW_HEIGHT = 14;
    static const int MAX_VISIBLE_ROWS = 9;   // header + 8 rows (callsign box below)

    // Right panel - Radar display
    static const int RADAR_X = 126;
    static const int RADAR_Y = 16;
    static const int RADAR_W = 194;  // Right 194 pixels for radar
    static const int RADAR_H = 154;
    static const int RADAR_CENTER_X = 223;  // 126 + 194/2 = 223
    static const int RADAR_CENTER_Y = 85;   // fills the screen height
    static const int RADAR_RADIUS = 75;     // N/S sit outside the ring
    static const int NUM_RANGE_RINGS = 3;
    static const int CALLSIGN_BOX_Y = 144;
    static const int NUM_TRAIL_LINES = 4;

    /**
     * Draw range rings
     * @param cx Center X
     * @param cy Center Y
     * @param radius Outer radius
     * @param num_rings Number of rings
     * @param max_range Maximum range for labels
     * @param rotation_deg Rotation of the compass rose (heading at the top)
     */
    void drawRangeRings(int cx, int cy, int radius, int num_rings,
                        float max_range, float rotation_deg = 0.0f);

    /**
     * Draw compass markings (N/E/S/W)
     * @param cx Center X
     * @param cy Center Y
     * @param radius Radar radius
     * @param rotation_deg Rotation of the compass rose (heading at the top)
     */
    void drawCompassMarkings(int cx, int cy, int radius,
                             float rotation_deg = 0.0f);
    void drawRingLabels(int cx, int cy, int radius, int num_rings, float max_range);
    void drawScaledText(int x, int y, const char* text, uint16_t color, int scale);

    // Anti-aliased font for the selected callsign (loaded lazily).
    mutable TtfText callsign_font_;
    mutable bool callsign_font_loaded_ = false;
    mutable TtfText ui_font_;
    mutable bool ui_font_loaded_ = false;

    /**
     * Draw sweep line at current angle
     * @param cx Center X
     * @param cy Center Y
     * @param radius Radar radius
     * @param angle Sweep angle (0-360°)
     */
    void drawSweep(int cx, int cy, int radius, float angle);

    /**
     * Draw fading trail behind sweep line
     * @param cx Center X
     * @param cy Center Y
     * @param radius Radar radius
     * @param angle Current sweep angle
     */

    /**
     * Draw aircraft blip with heading vector
     * @param cx Center X
     * @param cy Center Y
     * @param distance Distance in km
     * @param bearing Bearing in degrees
     * @param heading Heading in degrees (0=N, clockwise)
     * @param velocity_valid Whether heading is meaningful
     * @param max_range Maximum range for scaling
     * @param selected Whether this aircraft is selected
     * @param rotation_deg Rotation of the compass rose (heading at the top)
     * @param radius Scope radius in pixels
     */
    void drawAircraftBlip(int cx, int cy, float distance, float bearing,
                         float heading, bool velocity_valid,
                         float max_range, bool selected,
                         float rotation_deg = 0.0f,
                         int radius = RADAR_RADIUS);

    /**
     * Draw circle outline (for range rings)
     * @param cx Center X
     * @param cy Center Y
     * @param radius Circle radius
     * @param color Line color
     */
    void drawCircleApprox(int cx, int cy, int radius, uint16_t color);

    /**
     * Draw dashed circle outline (for inner range rings)
     * @param cx Center X
     * @param cy Center Y
     * @param radius Circle radius
     * @param color Line color
     */
    void drawDashedCircleApprox(int cx, int cy, int radius, uint16_t color);

    /**
     * Blend two colors with alpha
     * @param color1 First color (RGB565)
     * @param color2 Second color (RGB565)
     * @param alpha Blend factor (0.0 = color2, 1.0 = color1)
     * @return Blended color (RGB565)
     */
    uint16_t blendColor(uint16_t color1, uint16_t color2, float alpha);

    /**
     * Convert polar coordinates to Cartesian screen coordinates
     * @param distance Distance in km
     * @param bearing Bearing in degrees (0° = North, clockwise)
     * @param max_range Maximum range for scaling
     * @param radius Scope radius in pixels
     * @param out_x Output X coordinate
     * @param out_y Output Y coordinate
     */
    void polarToCartesian(float distance, float bearing, float max_range,
                         int radius, int* out_x, int* out_y);
};

#endif // RADAR_UI_RENDERER_H
