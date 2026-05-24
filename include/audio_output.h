#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <alsa/asoundlib.h>
#include <vector>
#include <cstdint>

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool init(const char* device_name = "default",
              uint32_t sample_rate = 48000,
              int channels = 1);
    void close();

    bool isOpen() const { return pcm_handle != nullptr; }

    // Write audio samples to playback buffer (non-blocking)
    bool write(const std::vector<int16_t>& samples);

    // Available frames in ALSA buffer (to avoid overflow)
    snd_pcm_sframes_t getAvailableFrames();

private:
    snd_pcm_t* pcm_handle;
    uint32_t sample_rate;
    int channels;
    snd_pcm_uframes_t period_size;
    snd_pcm_uframes_t buffer_size;

    bool recover(int err);
};

#endif // AUDIO_OUTPUT_H
