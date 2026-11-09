#include "core/beep_player.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;
constexpr float kTwoPi = 6.28318530718f;
}  // namespace

bool BeepPlayer::ensureOpen() {
    if (open_) return true;
    open_ = out_.init("default", kSampleRate, 1);
    if (open_) out_.setQuiet(true);
    return open_;
}

void BeepPlayer::close() {
    if (open_) {
        out_.close();
        open_ = false;
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

    out_.write(samples);
}
