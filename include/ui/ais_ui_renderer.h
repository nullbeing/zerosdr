#ifndef AIS_UI_RENDERER_H
#define AIS_UI_RENDERER_H

#include "ais/ais_store.h"
#include "ui/base_ui_renderer.h"

#include <cstdint>
#include <vector>

/**
 * AisUIRenderer - "SONAR" themed AIS display.
 *
 * The list uses the 8x16 font, the console uses the denser 6x8 font. The
 * palette is the marine phosphor scheme: deep blue-black, cyan, amber for
 * selection and dim blue-grey for secondary text.
 */
class MapBackground;

class AisUIRenderer : public BaseUIRenderer {
public:
    explicit AisUIRenderer(Framebuffer& fb);

    void renderList(const std::vector<ais::VesselState>& vessels,
                    int selected_index, int scroll_offset,
                    uint64_t total_messages, float messages_per_second,
                    int gain_tenths, bool agc);

    void renderConsole(const std::vector<ais::AisLogEntry>& entries,
                       int scroll_offset, uint64_t total_messages,
                       float messages_per_second, int gain_tenths, bool agc,
                       size_t vessels);

    void renderPlot(const std::vector<ais::VesselState>& vessels,
                    int selected_index, float range_nm, bool station_valid,
                    double station_latitude, double station_longitude,
                    uint64_t total_messages, float messages_per_second,
                    int gain_tenths, bool agc, MapBackground* map = nullptr);

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

    void renderStatusBar(uint64_t total_messages, float messages_per_second,
                         int gain_tenths, bool agc, size_t vessels);
    void renderStatsBar(uint64_t total_messages, size_t vessels,
                        int gain_tenths, bool agc);

    Theme theme_;
};

#endif  // AIS_UI_RENDERER_H
