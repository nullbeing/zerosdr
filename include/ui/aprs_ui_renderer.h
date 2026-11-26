#ifndef APRS_UI_RENDERER_H
#define APRS_UI_RENDERER_H

#include "aprs/aprs_store.h"
#include "ui/base_ui_renderer.h"

#include <cstdint>
#include <vector>

class MapBackground;

/**
 * AprsUIRenderer - green "packet terminal" styling for the APRS page.
 *
 * The list and detail views use the 8x16 font; the console and the small
 * labels use the denser 6x8 font.
 */
class AprsUIRenderer : public BaseUIRenderer {
public:
    explicit AprsUIRenderer(Framebuffer& fb);

    void renderList(const std::vector<aprs::StationState>& stations,
                    int selected_index, int scroll_offset,
                    uint64_t total_packets, float packets_per_minute,
                    int gain_tenths, bool agc, uint32_t frequency);

    void renderMap(const std::vector<aprs::StationState>& stations,
                   int selected_index, float range_nm, bool station_valid,
                   double station_latitude, double station_longitude,
                   uint64_t total_packets, float packets_per_minute,
                   int gain_tenths, bool agc, uint32_t frequency,
                   MapBackground* map);

    void renderDetail(const aprs::StationState* station, size_t station_count,
                      uint64_t total_packets, float packets_per_minute,
                      int gain_tenths, bool agc, uint32_t frequency);

    void renderConsole(const std::vector<aprs::LogEntry>& entries,
                       int scroll_offset, uint64_t total_packets,
                       float packets_per_minute, int gain_tenths, bool agc,
                       size_t station_count, uint32_t frequency);

    void renderHelp();

    void drawChar(int x, int y, char c, uint16_t color) override;
    void drawText(int x, int y, const char* text, uint16_t color) override;
    void drawTextBg(int x, int y, const char* text, uint16_t fg,
                    uint16_t bg) override;
    int getTextWidth(const char* text) const override;

    void drawSmallText(int x, int y, const char* text, uint16_t color);
    int smallTextWidth(const char* text) const;

    uint16_t backgroundColor() const { return theme_.bg; }

private:
    struct Theme {
        uint16_t bg;
        uint16_t panel;
        uint16_t grid;
        uint16_t text;
        uint16_t dim;
        uint16_t accent;
        uint16_t selected;
        uint16_t alarm;
        uint16_t status_bg;
        uint16_t status_text;
    };

    void renderStatusBar(uint64_t total_packets, float packets_per_minute,
                         int gain_tenths, bool agc, size_t stations,
                         uint32_t frequency);
    void renderStatsBar(uint64_t total_packets, size_t stations,
                        int gain_tenths, bool agc, const char* hint);
    void renderTrackPlot(const aprs::StationState& station, int x, int y,
                         int w, int h);

    Theme theme_;
};

#endif  // APRS_UI_RENDERER_H
