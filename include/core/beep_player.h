#ifndef BEEP_PLAYER_H
#define BEEP_PLAYER_H

#include "audio_output.h"

/**
 * BeepPlayer - tiny sine-tone player for UI feedback.
 *
 * Opens the default ALSA device lazily on the first beep and is closed when
 * the owning page exits, so it never holds the speaker while the SDR audio
 * path is active.
 */
class BeepPlayer {
public:
    void close();

    /**
     * Play a short windowed sine.
     * @param freq_hz    tone frequency
     * @param duration_ms length in milliseconds
     * @param amplitude  0.0 .. 1.0 of full scale
     */
    void play(float freq_hz, int duration_ms, float amplitude);

private:
    bool ensureOpen();

    AudioOutput out_;
    bool open_ = false;
};

#endif // BEEP_PLAYER_H
