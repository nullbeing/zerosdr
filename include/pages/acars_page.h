#ifndef ACARS_PAGE_H
#define ACARS_PAGE_H

#include "acars/acars_controller.h"
#include "core/ipage.h"
#include "framebuffer.h"
#include "input_handler.h"
#include "sdr_controller.h"
#include "ui/acars_ui_renderer.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class AcarsPage : public IPage {
public:
    AcarsPage(Framebuffer& fb, SDRController& sdr, InputHandler& input,
              AcarsUIRenderer& ui);
    ~AcarsPage() override;

    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    const char* getName() const override { return "ACARS"; }
    const char* getDescription() const override {
        return "ACARS Aircraft Messages";
    }

private:
    enum class View { List, Detail };

    void loadConfig();
    void startDecoder();
    void restartDecoder();
    void render();
    int indexOfSeq(const std::vector<acars::Message>& messages,
                   uint64_t seq) const;
    void stepDetail(const std::vector<acars::Message>& messages, int delta);

    Framebuffer& fb_;
    SDRController& sdr_;
    InputHandler& input_;
    AcarsUIRenderer& ui_;
    acars::AcarsController controller_;

    View view_ = View::List;
    int selected_index_ = 0;
    uint64_t detail_seq_ = 0;
    int detail_scroll_ = 0;
    bool help_visible_ = false;

    std::vector<float> channels_;
    float gain_db_ = 40.0f;
    int udp_port_ = 5556;

    std::chrono::steady_clock::time_point last_render_{};
};

#endif  // ACARS_PAGE_H
