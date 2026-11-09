#ifndef RADAR_UI_RENDERER_H
#define RADAR_UI_RENDERER_H

#include "ui/base_ui_renderer.h"
#include "adsb/adsb_message.h"
#include <vector>
#include <cmath>

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
class RadarUIRenderer : public BaseUIRenderer {
public:
    explicit RadarUIRenderer(Framebuffer& fb);
    ~RadarUIRenderer();

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
     */
    void renderRadarDisplay(const std::vector<AircraftState>& aircraft,
                           float sweep_angle,
                           float max_range,
                           double observer_lat,
                           double observer_lon,
                           int selected_index,
                           int scroll_offset,
                           bool blink_state);

    /**
     * Render top status bar
     * @param aircraft_count Number of aircraft
     * @param max_range Maximum range (km)
     * @param gain Current gain value
     */
    void renderStatusBar(int aircraft_count, float max_range, float gain);

    /**
     * Render aircraft list in left panel
     * @param aircraft Vector of aircraft to display
     * @param selected_index Index of selected aircraft
     * @param scroll_offset Scroll offset for list
     */
    void renderAircraftList(const std::vector<AircraftState>& aircraft,
                           int selected_index,
                           int scroll_offset);

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
    // Layout constants (320x170 display) - Left-right split layout
    static const int STATUS_BAR_Y = 0;
    static const int STATUS_BAR_H = 16;

    // Left panel - Aircraft list
    static const int LIST_X = 0;
    static const int LIST_Y = 16;
    static const int LIST_W = 110;  // Left 110 pixels for list
    static const int LIST_H = 154;  // 170 - 16 = 154
    static const int ROW_HEIGHT = 14;
    static const int MAX_VISIBLE_ROWS = 11;  // 154 / 14 = 11 rows

    // Right panel - Radar display
    static const int RADAR_X = 110;
    static const int RADAR_Y = 16;
    static const int RADAR_W = 210;  // Right 210 pixels for radar
    static const int RADAR_H = 154;
    static const int RADAR_CENTER_X = 215;  // 110 + 210/2 = 215
    static const int RADAR_CENTER_Y = 93;   // 16 + 154/2 = 93
    static const int RADAR_RADIUS = 70;
    static const int NUM_RANGE_RINGS = 3;
    static const int NUM_TRAIL_LINES = 4;

    /**
     * Draw range rings
     * @param cx Center X
     * @param cy Center Y
     * @param radius Outer radius
     * @param num_rings Number of rings
     * @param max_range Maximum range for labels
     */
    void drawRangeRings(int cx, int cy, int radius, int num_rings, float max_range);

    /**
     * Draw compass markings (N/E/S/W)
     * @param cx Center X
     * @param cy Center Y
     * @param radius Radar radius
     */
    void drawCompassMarkings(int cx, int cy, int radius);

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
     */
    void drawAircraftBlip(int cx, int cy, float distance, float bearing,
                         float heading, bool velocity_valid,
                         float max_range, bool selected);

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
     * @param out_x Output X coordinate
     * @param out_y Output Y coordinate
     */
    void polarToCartesian(float distance, float bearing, float max_range,
                         int* out_x, int* out_y);
};

#endif // RADAR_UI_RENDERER_H
