#ifndef SCANNER_VIEW_H
#define SCANNER_VIEW_H

#include "framebuffer.h"
#include "scanner/scanner_dsp.h"

#include <cstdint>
#include <string>
#include <vector>

// What the scanner is doing right now.
enum class ScannerState { Scan, Lock, Hang, Hold, Error };

// One row of the signal list (already sorted high -> low frequency).
struct ScannerRow {
    uint32_t id = 0;
    uint32_t hz = 0;
    float snr_db = 0.0f;
    double age_s = 0.0;       // seconds since last heard
    double duration_s = 0.0;  // length of the current/last burst
    bool tuned = false;
    bool locked_out = false;
    // Bit i set = heard i seconds ago (bit 0 = this second).
    uint64_t heard = 0;
};

// Everything the view needs for one frame. Filled by ScannerPage under its
// lock, then drawn without touching any shared state.
struct ScannerSnapshot {
    ScannerState state = ScannerState::Scan;
    std::string error;

    uint32_t start_hz = 118000000;
    uint32_t stop_hz = 137000000;
    std::string preset;
    scanner::Mode mode = scanner::Mode::AM;
    bool all_mode = false;     // sweeping every preset in turn
    int band_count = 0;        // presets in the ALL rotation
    std::string band;          // preset being swept / tuned in ALL mode

    uint32_t tuned_hz = 0;     // 0 while sweeping
    uint32_t tuned_id = 0;
    uint32_t sweep_hz = 0;
    int sweep_progress = 0;    // 0..100
    bool active = false;       // carrier present on the tuned frequency
    float snr_db = 0.0f;
    float af_level = 0.0f;     // 0..1
    double duration_s = 0.0;
    double hang_left_s = 0.0;

    float floor_db = 0.0f;
    float threshold_db = 10.0f;
    int gain_tenths = -1;      // -1 = auto
    float hang_s = 2.0f;
    bool muted = false;
    int lockout_count = 0;

    std::vector<ScannerRow> rows;
    std::vector<float> scope;  // most recent audio, -1..1

    std::string clock;         // "HH:MM:SS"
    int battery_percent = -1;  // -1 = unknown
    std::string toast;
};

struct ScannerEditorState {
    std::string preset;        // shown on the PRESET row
    std::string start;
    std::string stop;
    std::string step;          // channel step, e.g. "12.5"
    scanner::Mode mode = scanner::Mode::AM;
    int field = 0;  // 0 preset, 1 start, 2 stop, 3 mode, 4 step
    bool all = false;          // ALL selected: the fields are not used
    bool adding = false;       // + ADD NEW selected
    bool user_preset = false;  // a preset the user added (can be deleted)
    std::vector<std::string> all_names;  // presets in the ALL rotation
    std::string info;          // hint line
    bool info_alert = false;   // draw the hint as a warning
    std::string error;
};

// Cassette-futurism amber renderer for the Scanner page (320x170).
class ScannerView {
public:
    explicit ScannerView(Framebuffer& fb) : fb_(fb) {}

    void draw(const ScannerSnapshot& snap);
    void drawHelp();
    void drawEditor(const ScannerEditorState& editor);

    static constexpr int kVisibleRows = 7;

private:
    void drawTopBar(const ScannerSnapshot& snap);
    void drawSignals(const ScannerSnapshot& snap);
    void drawScope(const ScannerSnapshot& snap);
    void drawParams(const ScannerSnapshot& snap);

    void text(int x, int y, const char* s, uint16_t color);
    void textBig(int x, int y, const char* s, uint16_t color);
    int textWidth(const char* s) const;
    void rule(int y);
    void hatch(int x, int y, int w, int h, uint16_t color);
    void tag(int x, int cy, int w, bool filled, uint16_t color);
    void segmentBar(int x, int y, int w, int h, float fraction, uint16_t on,
                    uint16_t off);
    void brackets(int x, int y, int w, int h, uint16_t color);

    Framebuffer& fb_;
};

#endif  // SCANNER_VIEW_H
