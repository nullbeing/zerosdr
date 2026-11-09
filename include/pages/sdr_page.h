#ifndef SDR_PAGE_H
#define SDR_PAGE_H

#include "core/ipage.h"
#include "framebuffer.h"
#include "input_handler.h"
#include "sdr_controller.h"
#include "fft_processor.h"
#include "ui/sdr_ui_renderer.h"
#include "audio_demodulator.h"
#include "audio_output.h"
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>

/**
 * SDRPage - SDR spectrum analyzer page
 *
 * Encapsulates all SDR spectrum analyzer functionality including:
 * - Real-time spectrum and waterfall display
 * - Audio demodulation (FM/NFM/AM)
 * - Frequency tuning and gain control
 * - Audio volume and squelch control
 */
class SDRPage : public IPage {
public:
    SDRPage(Framebuffer& fb, SDRController& sdr, AudioOutput& audio,
            SDRUIRenderer& ui, InputHandler& input);
    ~SDRPage();

    // IPage interface implementation
    bool init() override;
    void cleanup() override;
    void onEnter() override;
    void onExit() override;
    void update(float delta_time) override;
    bool handleInput(AppKeyCode key) override;
    const char* getName() const override { return "SDR"; }
    const char* getDescription() const override { return "SDR Spectrum Analyzer"; }

private:
    // References to shared services
    Framebuffer& fb_;
    SDRController& sdr_;
    AudioOutput& audio_;
    SDRUIRenderer& ui_;
    InputHandler& input_;

    // SDR-specific components
    FFTProcessor fft_;
    AudioDemodulator demod_;

    // SDR state variables
    uint32_t center_freq_;
    uint32_t sample_rate_;
    int gain_;
    float volume_;
    float squelch_;              // Current (active) squelch threshold
    int demod_mode_;  // 0=FM, 1=NFM, 2=AM, 3=USB, 4=LSB
    std::atomic<bool> audio_enabled_;

    // Audio runs on its own thread so UI/FFT work cannot starve ALSA.
    std::thread audio_thread_;
    std::atomic<bool> audio_thread_running_;
    std::mutex demod_mutex_;

    // Per-mode squelch memory: each demod mode remembers its own squelch
    // so switching modes (D key) doesn't reset a carefully tuned NFM squelch
    // when going to AM, etc. squelch_ mirrors squelch_values_[demod_mode_].
    static constexpr int NUM_DEMOD_MODES = 5;
    float squelch_values_[NUM_DEMOD_MODES];

    // Sample rate presets
    static const uint32_t sample_rates_[];
    static const int num_sample_rates_;
    int rate_index_;

    // FFT size presets
    static const size_t fft_sizes_[];
    static const int num_fft_sizes_;
    int fft_index_;

    // Display bandwidth presets (zoom levels)
    static const uint32_t display_bw_[];
    static const int num_display_bw_;
    int bw_index_;
    uint32_t display_span_;

    // Demodulation bandwidth presets
    static const uint32_t fm_bandwidths_[];
    static const uint32_t nfm_bandwidths_[];
    static const uint32_t am_bandwidths_[];
    static const uint32_t ssb_bandwidths_[];
    int fm_bw_index_;
    int nfm_bw_index_;
    int am_bw_index_;
    int ssb_bw_index_;

    // Frequency input buffer
    char freq_input_buffer_[16];
    int freq_input_len_;

    // Sample buffers
    std::vector<uint8_t> fft_samples_;
    std::vector<float> magnitude_;
    std::vector<int16_t> audio_samples_;

    // Frame rate control
    float frame_accumulator_;
    static constexpr float FRAME_INTERVAL_SEC = 1.0f / 30.0f;  // ~30 FPS render cap

    // Deemphasis time constant (50 or 75 us)
    float deemphasis_tau_us_;

    // Tuning step (0 = Auto, then explicit steps)
    // Auto picks a sensible default per demod mode (see getAutoStep).
    int step_index_;
    static const uint32_t step_sizes_[];
    static const int num_step_sizes_;

    // H key: full-screen key reference + settings tab
    bool help_visible_ = false;
    int help_tab_ = 0;        // 0 = key list, 1 = settings
    int settings_index_ = 0;  // highlighted row on the settings tab
    bool audio_agc_ = true;   // audio AGC (moved out of the hotkeys)
    uint32_t saved_sample_rate_ = 0;  // from sdr_settings.json, 0 = not set

    // Demod mode names
    static const char* demod_names_[];

    // Helper methods
    uint32_t getCurrentDemodBandwidth() const;
    int maxSpanIndex() const;              // widest zoom level that fits the sample rate
    void setSpanToSampleRate();            // reset zoom to the full captured band
    uint32_t getCurrentStep() const;       // Effective step in Hz (resolves Auto)
    uint32_t getAutoStep() const;          // Default step for current demod mode
    const char* getStepLabel() const;      // "AUTO", "100", "1.0k", "100k", etc.
    void renderFrame();
    void handleOverlayInput(AppKeyCode key);
    void cycleSetting(int index);
    void loadSettings();
    void saveSettings() const;
    void handleFrequencyInput(AppKeyCode key);
    void handleNavigationKeys(AppKeyCode key);
    void handleAudioControls(AppKeyCode key);
    void handleModeControls(AppKeyCode key);

    void startAudioThread();
    void stopAudioThread();
    void audioLoop();
};

#endif // SDR_PAGE_H
