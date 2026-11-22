#ifndef ACARS_UI_RENDERER_H
#define ACARS_UI_RENDERER_H

#include "acars/acars_controller.h"
#include "framebuffer.h"

#include <cstdint>
#include <string>
#include <vector>

class AcarsUIRenderer {
public:
    explicit AcarsUIRenderer(Framebuffer& fb);

    void renderList(const std::vector<acars::Message>& messages,
                    int selected_index, uint64_t total, float rate,
                    const std::vector<float>& channels, float gain_db,
                    bool running, const std::string& error);
    void renderDetail(const acars::Message& message, int scroll);
    void renderHelp();
    void renderDialog(const char* title, const char* message,
                      const char* hint);

private:
    void drawChar(int x, int y, char c, uint16_t color);
    void drawText(int x, int y, const char* text, uint16_t color);
    void drawSmall(int x, int y, const char* text, uint16_t color);
    int smallWidth(const char* text) const;
    void drawPanel(int x, int y, int w, int h, uint16_t color);

    Framebuffer& fb_;

    static constexpr uint16_t kBg = 0x0841;
    static constexpr uint16_t kPanel = 0x10A2;
    static constexpr uint16_t kGrid = 0x2124;
    static constexpr uint16_t kDim = 0x8410;
    static constexpr uint16_t kText = 0xCE79;
    static constexpr uint16_t kAccent = 0xFD20;
    static constexpr uint16_t kGreen = 0x07E0;
    static constexpr uint16_t kRed = 0xF800;
};

#endif  // ACARS_UI_RENDERER_H
