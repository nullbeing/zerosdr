#ifndef BEEP_PLAYER_H
#define BEEP_PLAYER_H

#include "audio_output.h"

#include <atomic>
#include <mutex>
#include <thread>

/**
 * BeepPlayer - tiny sine-tone player for UI feedback.
 *
 * Opens the default ALSA device lazily on the first beep and is closed when
 * the owning page exits, so it never holds the speaker while the SDR audio
 * path is active.
 */
class BeepPlayer {
public:
    ~BeepPlayer();

    void close();

    /** Start or retune a continuous tone (variometer). */
    void setTone(float freq_hz, float amplitude);

    /** Start or retune a pulsed tone: short dots at a fixed period. */
    void setPulse(float freq_hz, int period_ms, int on_ms, float amplitude);

    /** Fade out and stop the continuous tone. */
    void stopTone();

    /**
     * Play a short windowed sine.
     * @param freq_hz    tone frequency
     * @param duration_ms length in milliseconds
     * @param amplitude  0.0 .. 1.0 of full scale
     */
    void play(float freq_hz, int duration_ms, float amplitude);

private:
    bool ensureOpen();
    void toneLoop();

    AudioOutput out_;
    bool open_ = false;

    std::thread tone_thread_;
    std::atomic<bool> tone_active_{false};
    std::atomic<float> tone_freq_{440.0f};
    std::atomic<float> tone_amp_{0.0f};
    std::atomic<int> pulse_period_ms_{0};  // 0 = continuous
    std::atomic<int> pulse_on_ms_{0};
    std::mutex write_mutex_;
};

#endif // BEEP_PLAYER_H
