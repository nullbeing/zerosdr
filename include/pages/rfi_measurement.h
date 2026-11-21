#ifndef RFI_MEASUREMENT_H
#define RFI_MEASUREMENT_H
#include <algorithm>
#include <cmath>
#include <vector>

// Own the reference in linear power, independent of display smoothing.
class RfiReference {
public:
    static constexpr unsigned kSweeps = 8;
    void reset() { reference_.clear(); count_ = 0; collecting_ = false; }
    void begin() { reset(); collecting_ = true; }
    void add(const std::vector<float>& power) {
        if (!collecting_ || power.empty()) return;
        if (reference_.empty()) reference_.assign(power.size(), 0.0f);
        if (reference_.size() != power.size()) { reset(); return; }
        for (size_t i = 0; i < power.size(); ++i) reference_[i] += power[i] / kSweeps;
        if (++count_ == kSweeps) collecting_ = false;
    }
    bool ready() const { return !reference_.empty() && count_ == kSweeps; }
    bool collecting() const { return collecting_; }
    unsigned count() const { return count_; }
    const std::vector<float>& power() const { return reference_; }
    static float db(float power) { return 10.0f * std::log10(std::max(power, 1e-20f)); }
    // Existing FFTProcessor returns 20*log10(power); undo that here without
    // changing the existing SDR/Spectrum display calibration.
    static float fftPower(float magnitude, size_t fft_size) {
        const float scale = static_cast<float>(fft_size) / 2.0f;
        return std::pow(10.0f, magnitude / 20.0f) / (scale * scale);
    }
    float medianChange(const std::vector<float>& live) const {
        if (!ready() || live.size() != reference_.size()) return 0;
        std::vector<float> changes(live.size());
        for (size_t i = 0; i < live.size(); ++i) changes[i] = db(live[i]) - db(reference_[i]);
        auto mid = changes.begin() + changes.size() / 2;
        std::nth_element(changes.begin(), mid, changes.end());
        return *mid;
    }
private:
    std::vector<float> reference_;
    unsigned count_ = 0;
    bool collecting_ = false;
};
#endif
