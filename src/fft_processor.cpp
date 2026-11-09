#include "fft_processor.h"
#include <cmath>
#include <algorithm>

FFTProcessor::FFTProcessor(size_t fft_size)
    : fft_size(fft_size),
      // RTL-SDR Blog V4 typical: ~2-3% amplitude + ~2-3° phase imbalance.
      // ε ≈ 0.03+0.03j gives ~15 dB additional mirror suppression on top
      // of the hardware's intrinsic ~30 dBc.
      iq_eps_(0.03f, 0.03f),
      iq_correction_enabled_(true) {
    fft_input.resize(fft_size);
    fft_output.resize(fft_size);
    rebuildTables();
}

FFTProcessor::~FFTProcessor() {
}

void FFTProcessor::setFFTSize(size_t new_size) {
    fft_size = new_size;
    fft_input.resize(fft_size);
    fft_output.resize(fft_size);
    rebuildTables();
}

void FFTProcessor::rebuildTables() {
    window.resize(fft_size);
    for (size_t i = 0; i < fft_size; i++) {
        window[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (fft_size - 1)));
    }

    // Twiddle factors: twiddles[k] = exp(-2*pi*i*k / N), N = fft_size/2
    // Each level reuses them via stride indexing; pre-computing all avoids std::polar in hot path
    size_t half = fft_size / 2;
    twiddles.resize(half);
    for (size_t k = 0; k < half; k++) {
        twiddles[k] = std::polar(1.0f, -2.0f * (float)M_PI * k / fft_size);
    }

    // Bit-reversal permutation table
    bit_reverse.resize(fft_size);
    size_t log_n = 0;
    for (size_t tmp = fft_size; tmp > 1; tmp >>= 1) log_n++;
    for (size_t i = 0; i < fft_size; i++) {
        size_t rev = 0;
        size_t x = i;
        for (size_t b = 0; b < log_n; b++) {
            rev = (rev << 1) | (x & 1);
            x >>= 1;
        }
        bit_reverse[i] = rev;
    }
}

void FFTProcessor::applyWindow() {
    for (size_t i = 0; i < fft_size; i++) {
        fft_input[i] *= window[i];
    }
}

void FFTProcessor::applyIQImbalanceCorrection() {
    // Mirror image at bin k is at bin (fft_size - k).
    // Process in pairs to avoid clobbering: read both, write both.
    // Skip DC (0) and Nyquist (fft_size/2): those have no physical mirror.
    for (size_t k = 1; k < fft_size / 2; k++) {
        size_t nk = fft_size - k;
        std::complex<float> y_k  = fft_output[k];
        std::complex<float> y_nk = fft_output[nk];
        fft_output[k]  = y_k  - iq_eps_ * std::conj(y_nk);
        fft_output[nk] = y_nk - iq_eps_ * std::conj(y_k);
    }
}

void FFTProcessor::computeFFTInPlace() {
    // Bit-reversal permutation
    for (size_t i = 0; i < fft_size; i++) {
        fft_output[i] = fft_input[bit_reverse[i]];
    }

    // Iterative Cooley-Tukey butterfly
    for (size_t size = 2; size <= fft_size; size <<= 1) {
        size_t half = size >> 1;
        size_t table_step = fft_size / size;
        for (size_t i = 0; i < fft_size; i += size) {
            for (size_t k = 0, j = 0; k < half; k++, j += table_step) {
                std::complex<float> t = twiddles[j] * fft_output[i + k + half];
                std::complex<float> u = fft_output[i + k];
                fft_output[i + k] = u + t;
                fft_output[i + k + half] = u - t;
            }
        }
    }
}

void FFTProcessor::process(const uint8_t* iq_data, size_t byte_count, std::vector<float>& magnitude) {
    // Convert IQ samples (uint8) to complex float
    size_t sample_count = std::min(byte_count / 2, fft_size);

    for (size_t i = 0; i < sample_count; i++) {
        // RTL-SDR outputs unsigned 8-bit IQ, centered at 127
        float i_val = (iq_data[i * 2] - 127.0f) / 128.0f;
        float q_val = (iq_data[i * 2 + 1] - 127.0f) / 128.0f;
        fft_input[i] = std::complex<float>(i_val, q_val);
    }

    // Zero-pad remainder (if any)
    for (size_t i = sample_count; i < fft_size; i++) {
        fft_input[i] = std::complex<float>(0.0f, 0.0f);
    }

    // Apply pre-computed Hann window
    applyWindow();

    // Compute FFT in-place
    computeFFTInPlace();

    // Suppress IQ mirror image (direct-conversion receiver artifact)
    if (iq_correction_enabled_) {
        applyIQImbalanceCorrection();
    }

    // Calculate magnitude in dB
    // log10(re^2 + im^2) saves the sqrt compared to log10(sqrt(re^2 + im^2))
    magnitude.resize(fft_size);
    const float log_norm = 20.0f / std::log(10.0f);  // convert ln to log10 via natural log identity
    for (size_t i = 0; i < fft_size; i++) {
        float real = fft_output[i].real();
        float imag = fft_output[i].imag();
        float power = real * real + imag * imag;
        magnitude[i] = log_norm * std::log(power + 1e-20f);
    }

    // Suppress DC spike (LO leakage dominates bin 0; replace with neighbor average)
    // Adjacent bins are unaffected by DC leakage and represent the true noise floor
    magnitude[0] = 0.5f * (magnitude[1] + magnitude[fft_size - 1]);

    // FFT shift - move DC to center
    std::rotate(magnitude.begin(), magnitude.begin() + fft_size / 2, magnitude.end());
}

void FFTProcessor::process(const std::vector<uint8_t>& iq_samples, std::vector<float>& magnitude) {
    process(iq_samples.data(), iq_samples.size(), magnitude);
}
