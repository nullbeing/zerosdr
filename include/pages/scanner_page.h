#ifndef SCANNER_PAGE_H
#define SCANNER_PAGE_H

#include "audio_output.h"
#include "core/ipage.h"
#include "fft_processor.h"
#include "framebuffer.h"
#include "input_handler.h"
#include "scanner/scanner_dsp.h"
#include "sdr_controller.h"
#include "ui/scanner_view.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Scanner page: sweeps a frequency range, lists every signal above the
// threshold, and parks on one to play it. When the tuned signal drops out the
// scanner waits the hang time, then moves to another live signal on its own.
//
// One worker thread owns the dongle (tuning + reading). The UI thread only
// reads a snapshot under `mutex_` and posts requests through atomics.
class ScannerPage : public IPage {
public:
    ScannerPage(Framebuffer& fb, SDRController& sdr, AudioOutput& audio,
                InputHandler& input);
    ~ScannerPage() override;

    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    void onVolumeChanged(int percent) override;
    const char* getName() const override { return "Scanner"; }
    const char* getDescription() const override {
        return "Signal scanner: finds and plays active channels";
    }

private:
    static constexpr uint32_t kSampleRate = scanner::Channelizer::kInputRate;
    static constexpr size_t kFftSize = 1024;
    static constexpr double kUsableFraction = 0.78;
    // While listening, the LO sits this far below the signal so the RTL-SDR
    // DC spike stays outside the channel.
    static constexpr double kListenOffsetHz = 200000.0;

    struct Preset {
        const char* name;
        uint32_t start_hz;
        uint32_t stop_hz;
        scanner::Mode mode;
        uint32_t raster_hz;
    };
    static const Preset kPresets[];
    static const int kPresetCount;

    // One range the worker sweeps: the single scan range, or one preset of
    // the ALL rotation.
    struct Band {
        std::string name;
        uint32_t start_hz = 0;
        uint32_t stop_hz = 0;
        uint32_t raster_hz = 5000;
        scanner::Mode mode = scanner::Mode::AM;
    };
    static constexpr int kMaxUserPresets = 16;

    // --- settings / persistence
    void loadSettings();
    void saveSettings();
    void markDirty();
    // Built-in presets followed by the user's own.
    std::vector<Band> presetList() const;

    // --- worker
    void startWorker();
    void stopWorker();
    void workerLoop();
    void handleRequests();
    bool sweepOnce();
    bool listenChunk();
    void tuneTo(uint32_t hz);
    void moveOn();
    void applyGain();
    bool readSamples(std::vector<uint8_t>& iq, size_t bytes);
    void measureFft(const std::vector<uint8_t>& iq, int frames,
                    std::vector<float>& db_out);
    scanner::SliceGeometry geometry(double lo_hz, const Band& band) const;
    void rebuildBands();
    int bandFor(uint32_t hz) const;
    scanner::Mode bandMode(int band) const;
    double now() const;

    // --- UI
    void fillSnapshot(ScannerSnapshot& snap);
    bool handleKey(AppKeyCode key);
    void handleEditorKey(AppKeyCode key);
    void openEditor();
    void closeEditor();
    void applyEditor();
    void selectEditorChoice(int choice);
    void syncEditorView();
    int editorChoiceCount() const;
    std::string nextUserName() const;
    void showToast(const std::string& text);
    void stepSelection(int direction);
    int readBattery();

    Framebuffer& fb_;
    SDRController& sdr_;
    AudioOutput& audio_;
    InputHandler& input_;
    ScannerView view_;
    FFTProcessor fft_;

    // Settings (UI thread writes, worker reads through atomics / restart).
    uint32_t start_hz_ = 118000000;
    uint32_t stop_hz_ = 137000000;
    uint32_t raster_hz_ = 5000;
    std::string preset_ = "AIR";
    bool all_mode_ = false;            // sweep every preset in turn
    std::vector<Band> user_presets_;
    std::atomic<int> mode_{0};
    std::atomic<float> threshold_db_{10.0f};
    std::atomic<int> gain_index_{0};  // 0 = auto, then gain_options_
    std::atomic<int> hang_index_{2};  // 2 s
    std::atomic<bool> muted_{false};
    std::vector<int> gain_options_;   // tuner gains in tenths of a dB
    int saved_gain_tenths_ = -1;
    bool settings_dirty_ = false;
    std::chrono::steady_clock::time_point dirty_since_{};
    static const float kHangTimes[];
    static const int kHangCount;

    // Requests from the UI to the worker.
    std::atomic<bool> req_hold_toggle_{false};
    std::atomic<bool> req_skip_{false};
    std::atomic<uint32_t> req_tune_hz_{0};
    std::atomic<bool> req_lockout_{false};
    std::atomic<bool> req_clear_lockouts_{false};

    // Worker state (guarded by mutex_ where the UI reads it).
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> run_{false};
    ScannerState state_ = ScannerState::Scan;
    std::string error_;
    scanner::SignalTable table_;
    scanner::Channelizer chan_;
    bool hold_ = false;
    uint32_t tuned_hz_ = 0;
    double lo_hz_ = 0.0;
    double last_active_ = 0.0;
    double lost_at_ = 0.0;
    bool active_ = false;
    float tuned_snr_ = 0.0f;
    float floor_db_ = 0.0f;
    float af_level_ = 0.0f;
    uint32_t sweep_hz_ = 0;
    int sweep_progress_ = 0;
    int applied_gain_index_ = -2;
    std::vector<float> scope_;
    bool lockouts_changed_ = false;
    // Ranges being swept. Rebuilt by the UI thread only while the worker is
    // stopped, so the worker reads them without locking.
    std::vector<Band> bands_;
    bool worker_all_ = false;
    std::vector<int> band_phase_;
    size_t band_index_ = 0;   // next band to sweep (worker only)
    int sweep_band_ = 0;      // band being swept (mutex_)
    int tuned_band_ = 0;      // band of tuned_hz_ (mutex_)
    std::vector<uint8_t> listen_iq_;
    std::vector<int16_t> listen_pcm_;

    std::chrono::steady_clock::time_point epoch_;

    // UI-only state.
    ScannerSnapshot snapshot_;
    float render_accum_ = 0.0f;
    bool help_visible_ = false;
    bool editing_ = false;
    ScannerEditorState editor_;
    // Choice on the PRESET row: 0 = ALL, 1..N = presetList(), N+1 = ADD,
    // -1 = custom range.
    int editor_choice_ = 0;
    bool editor_fresh_ = true;
    bool editor_delete_armed_ = false;
    uint32_t editor_raster_ = 5000;
    std::string toast_;
    std::chrono::steady_clock::time_point toast_until_{};
    int battery_ = -1;
    float battery_accum_ = 1e9f;
    AppKeyCode held_key_ = APPKEY_NONE;
    float repeat_accum_ = 0.0f;
};

#endif  // SCANNER_PAGE_H
