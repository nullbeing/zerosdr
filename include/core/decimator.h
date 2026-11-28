#ifndef DECIMATOR_H
#define DECIMATOR_H

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Integer-ratio IQ decimator for the narrow SDR modes.
 *
 * The RTL-SDR cannot sample below ~225 kHz, but shortwave tuning wants a
 * 48 kHz wide view. The front end therefore captures at 288 kHz (or 384 kHz)
 * and this filter removes everything above the new Nyquist frequency before
 * every `factor`-th sample is kept. It is a windowed-sinc low-pass evaluated
 * only at the output positions, so the cost stays at roughly
 * `taps * output_rate`.
 *
 * Input and output are the interleaved unsigned 8-bit IQ pairs the RTL-SDR
 * delivers. The streaming process() keeps its history between calls; a
 * factor of one is a plain copy.
 */
class Decimator {
public:
    /** Designs the filter for `input_rate` and an integer `factor`. */
    void configure(uint32_t input_rate, uint32_t factor);

    uint32_t factor() const { return factor_; }

    /** Number of taps in the low-pass (0 for a passthrough). */
    size_t taps() const { return coeff_.size(); }

    /**
     * Decimates interleaved IQ. The filter state continues across calls, so
     * consecutive audio chunks join without a click.
     */
    void process(const uint8_t* iq, size_t samples, std::vector<uint8_t>& out);

    void process(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        process(in.data(), in.size() / 2, out);
    }

private:
    uint32_t factor_ = 1;
    uint64_t processed_ = 0;  // input samples seen so far
    std::vector<float> coeff_;
    std::vector<float> work_i_;
    std::vector<float> work_q_;
    std::vector<float> history_i_;
    std::vector<float> history_q_;
};

#endif  // DECIMATOR_H
