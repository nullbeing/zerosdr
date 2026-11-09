#ifndef FFT_PROCESSOR_H
#define FFT_PROCESSOR_H

#include <vector>
#include <complex>
#include <cstdint>

class FFTProcessor {
public:
    FFTProcessor(size_t fft_size = 1024);
    ~FFTProcessor();

    // Process IQ samples and return magnitude spectrum
    void process(const std::vector<uint8_t>& iq_samples, std::vector<float>& magnitude);
    void process(const uint8_t* iq_data, size_t byte_count, std::vector<float>& magnitude);

    size_t getFFTSize() const { return fft_size; }
    void setFFTSize(size_t new_size);

    // IQ imbalance correction: applies ε·Y*(-f) subtraction in frequency domain
    // to suppress the mirror image inherent to direct-conversion receivers.
    // Default ε ≈ 0.03+0.03j works well for RTL-SDR Blog V4.
    void setIQImbalanceEpsilon(std::complex<float> eps) { iq_eps_ = eps; }
    std::complex<float> getIQImbalanceEpsilon() const { return iq_eps_; }

    void setIQImbalanceCorrection(bool enable) { iq_correction_enabled_ = enable; }
    bool getIQImbalanceCorrection() const { return iq_correction_enabled_; }

private:
    size_t fft_size;
    std::vector<std::complex<float>> fft_input;
    std::vector<std::complex<float>> fft_output;

    // Pre-computed lookup tables (rebuilt when fft_size changes)
    std::vector<std::complex<float>> twiddles;
    std::vector<size_t> bit_reverse;
    std::vector<float> window;

    // IQ imbalance compensation factor (see setIQImbalanceEpsilon)
    std::complex<float> iq_eps_;
    bool iq_correction_enabled_;

    // Rebuild all lookup tables for the current fft_size
    void rebuildTables();

    // Iterative in-place Cooley-Tukey FFT (operates on fft_output)
    void computeFFTInPlace();

    // Apply pre-computed Hann window to fft_input
    void applyWindow();

    // Mirror-image suppression: X[k] = Y[k] - ε·Y*[N-k]
    // Skips DC (bin 0) and Nyquist (bin N/2) where the concept doesn't apply.
    void applyIQImbalanceCorrection();
};

#endif // FFT_PROCESSOR_H
