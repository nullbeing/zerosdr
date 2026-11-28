#include "core/decimator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Taps per output sample. 32 gives a Blackman-windowed transition of roughly
// 0.17 * output_rate, which puts the stopband safely above the audio band.
constexpr size_t kTapsPerPhase = 32;

}  // namespace

void Decimator::configure(uint32_t input_rate, uint32_t factor) {
    (void)input_rate;
    factor_ = factor < 1 ? 1 : factor;
    processed_ = 0;
    coeff_.clear();
    history_i_.clear();
    history_q_.clear();
    work_i_.clear();
    work_q_.clear();
    if (factor_ <= 1) return;

    const size_t taps = kTapsPerPhase * factor_;
    // Cut at 0.42 of the *output* Nyquist-to-Nyquist span. The audio path
    // filters above 15 kHz afterwards, so the small transition band left
    // between 0.42 and 0.5 of the output rate is harmless.
    const double fc = 0.42 / static_cast<double>(factor_);

    coeff_.resize(taps);
    double sum = 0.0;
    const double last = static_cast<double>(taps - 1);
    for (size_t i = 0; i < taps; ++i) {
        const double t = static_cast<double>(i) - last / 2.0;
        const double sinc =
            std::fabs(t) < 1e-9 ? 2.0 * fc
                                : std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
        // Blackman window: -74 dB stopband, gentle passband droop.
        const double phase = 2.0 * M_PI * static_cast<double>(i) / last;
        const double window =
            0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
        coeff_[i] = static_cast<float>(sinc * window);
        sum += coeff_[i];
    }
    // Unity gain at DC so the 127 offset of the unsigned samples survives.
    for (float& c : coeff_) c = static_cast<float>(c / sum);

    history_i_.assign(taps - 1, 0.0f);
    history_q_.assign(taps - 1, 0.0f);
}

void Decimator::process(const uint8_t* iq, size_t samples,
                        std::vector<uint8_t>& out) {
    out.clear();
    if (samples == 0 || iq == nullptr) return;
    if (factor_ <= 1) {
        out.assign(iq, iq + samples * 2);
        return;
    }

    const size_t taps = coeff_.size();
    const size_t history = taps - 1;
    work_i_.resize(history + samples);
    work_q_.resize(history + samples);
    std::copy(history_i_.begin(), history_i_.end(), work_i_.begin());
    std::copy(history_q_.begin(), history_q_.end(), work_q_.begin());
    for (size_t i = 0; i < samples; ++i) {
        work_i_[history + i] = (iq[i * 2] - 127.0f) / 128.0f;
        work_q_[history + i] = (iq[i * 2 + 1] - 127.0f) / 128.0f;
    }

    // work[0] corresponds to absolute input index `base`; feed the filter
    // only samples whose whole window is real (no zero padding) and align
    // the output grid to the global sample counter.
    const int64_t base =
        static_cast<int64_t>(processed_) - static_cast<int64_t>(history);
    int64_t first = static_cast<int64_t>(processed_);
    if (first < static_cast<int64_t>(history)) {
        first = static_cast<int64_t>(history);
    }
    const int64_t remainder =
        ((first % static_cast<int64_t>(factor_)) + factor_) % factor_;
    if (remainder != 0) first += factor_ - remainder;

    out.reserve(static_cast<size_t>(samples / factor_) + 2);
    const float* h = coeff_.data();
    for (int64_t k = first; k < static_cast<int64_t>(processed_ + samples);
         k += factor_) {
        const size_t index = static_cast<size_t>(k - base);
        const float* wi = work_i_.data() + index;
        const float* wq = work_q_.data() + index;
        float acc_i = 0.0f;
        float acc_q = 0.0f;
        for (size_t t = 0; t < taps; ++t) {
            acc_i += h[t] * wi[-static_cast<ptrdiff_t>(t)];
            acc_q += h[t] * wq[-static_cast<ptrdiff_t>(t)];
        }
        const int vi = static_cast<int>(std::lround(acc_i * 128.0f + 127.0f));
        const int vq = static_cast<int>(std::lround(acc_q * 128.0f + 127.0f));
        out.push_back(static_cast<uint8_t>(std::clamp(vi, 0, 255)));
        out.push_back(static_cast<uint8_t>(std::clamp(vq, 0, 255)));
    }

    std::copy(work_i_.end() - static_cast<ptrdiff_t>(history),
              work_i_.end(), history_i_.begin());
    std::copy(work_q_.end() - static_cast<ptrdiff_t>(history),
              work_q_.end(), history_q_.begin());
    processed_ += samples;
}
