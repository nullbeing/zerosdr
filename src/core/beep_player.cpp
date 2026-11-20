#include "core/beep_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;
constexpr float kTwoPi = 6.28318530718f;
}  // namespace

BeepPlayer::~BeepPlayer() {
    close();
}

bool BeepPlayer::ensureOpen() {
    if (open_) return true;
    open_ = out_.init("default", kSampleRate, 1);
    if (open_) out_.setQuiet(true);
    return open_;
}

void BeepPlayer::close() {
    stopTone();
    if (open_) {
        out_.close();
        open_ = false;
    }
}

void BeepPlayer::setTone(float freq_hz, float amplitude) {
    if (freq_hz <= 0.0f || amplitude <= 0.0f) {
        stopTone();
        return;
    }
    if (!ensureOpen()) return;

    tone_freq_.store(freq_hz);
    tone_amp_.store(amplitude);
    pulse_period_ms_.store(0);

    if (tone_active_.exchange(true)) return;
    if (tone_thread_.joinable()) tone_thread_.join();
    tone_thread_ = std::thread(&BeepPlayer::toneLoop, this);
}

void BeepPlayer::setPulse(float freq_hz, int period_ms, int on_ms,
                          float amplitude) {
    if (freq_hz <= 0.0f || period_ms <= 0 || on_ms <= 0) {
        stopTone();
        return;
    }
    if (amplitude < 0.0f) amplitude = 0.0f;
    if (!ensureOpen()) return;

    tone_freq_.store(freq_hz);
    tone_amp_.store(amplitude);
    pulse_period_ms_.store(period_ms);
    pulse_on_ms_.store(on_ms);

    if (tone_active_.exchange(true)) return;
    if (tone_thread_.joinable()) tone_thread_.join();
    tone_thread_ = std::thread(&BeepPlayer::toneLoop, this);
}

void BeepPlayer::stopTone() {
    tone_active_.store(false);
    if (tone_thread_.joinable()) tone_thread_.join();
}

void BeepPlayer::toneLoop() {
    constexpr int kBlock = kSampleRate / 100;  // 10 ms
    constexpr double kTwoPiD = 6.283185307179586;
    std::vector<int16_t> samples(kBlock);

    double phase = 0.0;
    float amp = 0.0f;
    long long sample_index = 0;

    while (true) {
        const bool active = tone_active_.load();
        const float target_amp = active ? tone_amp_.load() : 0.0f;
        const float freq = tone_freq_.load();
        const int period_ms = pulse_period_ms_.load();
        const int on_ms = pulse_on_ms_.load();
        const int period_samples =
            (period_ms > 0) ? (kSampleRate * period_ms / 1000) : 0;
        const int on_samples =
            (on_ms > 0) ? (kSampleRate * on_ms / 1000) : 0;
        const int ramp = std::max(1, kSampleRate / 1000);  // 1 ms

        for (int i = 0; i < kBlock; ++i) {
            float envelope = target_amp;
            if (period_samples > 0) {
                const int pos = (int)(sample_index % period_samples);
                if (pos < on_samples) {
                    envelope = target_amp;
                    if (pos < ramp) {
                        envelope *= (float)pos / ramp;
                    } else if (pos > on_samples - ramp) {
                        envelope *= (float)(on_samples - pos) / ramp;
                    }
                } else {
                    envelope = 0.0f;
                }
            }

            // ~2 ms smoothing keeps the dots from clicking without blurring
            // their edges too much.
            amp += (envelope - amp) * 0.01f;
            const float s = std::sin((float)phase) * amp;
            samples[i] = static_cast<int16_t>(
                std::clamp(s, -1.0f, 1.0f) * 32767.0f);
            phase += kTwoPiD * freq / kSampleRate;
            if (phase >= kTwoPiD) phase -= kTwoPiD;
            ++sample_index;
        }

        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            out_.write(samples);
        }

        if (!active && amp < 0.0005f) break;
    }
}

void BeepPlayer::play(float freq_hz, int duration_ms, float amplitude) {
    if (duration_ms <= 0 || amplitude <= 0.0f) return;
    if (!ensureOpen()) return;

    const int frames = std::max(1, kSampleRate * duration_ms / 1000);
    const int fade = std::max(1, kSampleRate / 200);  // 5 ms attack/release

    std::vector<int16_t> samples(frames);
    for (int i = 0; i < frames; ++i) {
        float env = 1.0f;
        if (i < fade) {
            env = static_cast<float>(i) / fade;
        } else if (i >= frames - fade) {
            env = static_cast<float>(frames - i) / fade;
        }
        const float phase = kTwoPi * freq_hz * i / kSampleRate;
        const float s = std::sin(phase) * env * amplitude;
        samples[i] = static_cast<int16_t>(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
    }

    std::lock_guard<std::mutex> lock(write_mutex_);
    out_.write(samples);
}
