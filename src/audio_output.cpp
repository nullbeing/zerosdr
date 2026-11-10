#include "audio_output.h"
#include <iostream>
#include <cstring>

AudioOutput::AudioOutput()
    : pcm_handle(nullptr), sample_rate(48000), channels(1),
      period_size(2048), buffer_size(16384) {
}

AudioOutput::~AudioOutput() {
    close();
}

bool AudioOutput::init(const char* device_name, uint32_t sample_rate, int channels) {
    this->sample_rate = sample_rate;
    this->channels = channels;

    int err;

    // Open PCM device for playback. On images where the ALSA "default"
    // route is missing, fall back to the usual bridges: the PulseAudio
    // plugin (libasound2-plugins), PipeWire's own ALSA plugin, then the
    // codec card directly.
    err = snd_pcm_open(&pcm_handle, device_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        static const char* const kFallbacks[] = {
            "pulse", "pipewire", "plughw:1,0", "plughw:0,0",
        };
        const int primary_err = err;
        for (const char* alt : kFallbacks) {
            if (std::strcmp(alt, device_name) == 0) continue;
            err = snd_pcm_open(&pcm_handle, alt, SND_PCM_STREAM_PLAYBACK, 0);
            if (err >= 0) {
                std::cout << "Audio device: " << alt << " (fallback from "
                          << device_name << ")" << std::endl;
                break;
            }
        }
        if (err < 0) {
            std::cerr << "Cannot open audio device " << device_name << ": "
                      << snd_strerror(primary_err) << std::endl;
            return false;
        }
    }

    // Allocate hardware parameters object
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);

    // Fill with default values
    err = snd_pcm_hw_params_any(pcm_handle, hw_params);
    if (err < 0) {
        std::cerr << "Cannot initialize hardware parameters: "
                  << snd_strerror(err) << std::endl;
        close();
        return false;
    }

    // Set access type to interleaved
    err = snd_pcm_hw_params_set_access(pcm_handle, hw_params,
                                        SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        std::cerr << "Cannot set access type: " << snd_strerror(err) << std::endl;
        close();
        return false;
    }

    // Set sample format to signed 16-bit little endian
    err = snd_pcm_hw_params_set_format(pcm_handle, hw_params,
                                        SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        std::cerr << "Cannot set sample format: " << snd_strerror(err) << std::endl;
        close();
        return false;
    }

    // Set number of channels
    err = snd_pcm_hw_params_set_channels(pcm_handle, hw_params, channels);
    if (err < 0) {
        std::cerr << "Cannot set channel count: " << snd_strerror(err) << std::endl;
        close();
        return false;
    }

    // Set sample rate
    unsigned int actual_rate = sample_rate;
    err = snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &actual_rate, 0);
    if (err < 0) {
        std::cerr << "Cannot set sample rate: " << snd_strerror(err) << std::endl;
        close();
        return false;
    }
    if (actual_rate != sample_rate) {
        std::cerr << "Warning: requested rate " << sample_rate
                  << " Hz, got " << actual_rate << " Hz" << std::endl;
        this->sample_rate = actual_rate;
    }

    // Set period size (frames per interrupt)
    err = snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params,
                                                   &period_size, 0);
    if (err < 0) {
        std::cerr << "Cannot set period size: " << snd_strerror(err) << std::endl;
        close();
        return false;
    }

    // Set buffer size (total frames in buffer)
    err = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params,
                                                   &buffer_size);
    if (err < 0) {
        std::cerr << "Cannot set buffer size: " << snd_strerror(err) << std::endl;
        close();
        return false;
    }

    // Apply hardware parameters
    err = snd_pcm_hw_params(pcm_handle, hw_params);
    if (err < 0) {
        std::cerr << "Cannot set hardware parameters: "
                  << snd_strerror(err) << std::endl;
        close();
        return false;
    }

    // Get actual period and buffer sizes
    snd_pcm_hw_params_get_period_size(hw_params, &period_size, 0);
    snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);

    std::cout << "Audio output initialized: " << sample_rate << " Hz, "
              << channels << " channel(s), period=" << period_size
              << ", buffer=" << buffer_size << std::endl;

    // Prime the buffer with silence. The audio thread needs up to ~200 ms
    // to collect and demodulate its first chunk; without this cushion ALSA
    // reports a short burst of underruns at startup.
    std::vector<int16_t> silence(static_cast<size_t>(buffer_size) * channels, 0);
    write(silence);

    return true;
}

void AudioOutput::close() {
    if (pcm_handle != nullptr) {
        snd_pcm_drain(pcm_handle);
        snd_pcm_close(pcm_handle);
        pcm_handle = nullptr;
    }
}

bool AudioOutput::write(const std::vector<int16_t>& samples) {
    if (!pcm_handle || samples.empty()) {
        return false;
    }

    snd_pcm_sframes_t frames = samples.size() / channels;
    snd_pcm_sframes_t total = 0;
    int recoveries = 0;

    while (total < frames) {
        snd_pcm_sframes_t written = snd_pcm_writei(
            pcm_handle,
            samples.data() + total * channels,
            frames - total);

        if (written < 0) {
            if (written == -EPIPE) {
                if (!quiet_) std::cerr << "Audio buffer underrun" << std::endl;
                if (++recoveries > 4) return false;
                if (!recover(written)) return false;
                continue;
            } else if (written == -ESTRPIPE) {
                if (!quiet_) std::cerr << "Audio stream suspended" << std::endl;
                if (++recoveries > 4) return false;
                if (!recover(written)) return false;
                continue;
            } else {
                std::cerr << "Audio write error: " << snd_strerror(written) << std::endl;
                return false;
            }
        }

        if (written == 0) {
            break;
        }
        total += written;
    }

    if (total < frames) {
        std::cerr << "Warning: short write (expected " << frames
                  << ", wrote " << total << ")" << std::endl;
    }

    return total > 0;
}

snd_pcm_sframes_t AudioOutput::getAvailableFrames() {
    if (!pcm_handle) {
        return 0;
    }

    snd_pcm_sframes_t avail = snd_pcm_avail(pcm_handle);
    if (avail < 0) {
        recover(avail);
        return 0;
    }

    return avail;
}

bool AudioOutput::recover(int err) {
    if (err == -EPIPE) {
        // Buffer underrun
        err = snd_pcm_prepare(pcm_handle);
        if (err < 0) {
            std::cerr << "Cannot recover from underrun: "
                      << snd_strerror(err) << std::endl;
            return false;
        }
        return true;
    } else if (err == -ESTRPIPE) {
        // Stream suspended
        while ((err = snd_pcm_resume(pcm_handle)) == -EAGAIN) {
            usleep(100000); // Wait 100ms
        }
        if (err < 0) {
            err = snd_pcm_prepare(pcm_handle);
            if (err < 0) {
                std::cerr << "Cannot recover from suspend: "
                          << snd_strerror(err) << std::endl;
                return false;
            }
        }
        return true;
    }

    return false;
}
