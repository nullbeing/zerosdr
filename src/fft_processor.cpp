#include "fft_processor.h"
#include <cmath>
#include <algorithm>

FFTProcessor::FFTProcessor(size_t fft_size)
    : fft_size(fft_size) {
    fft_input.resize(fft_size);
    fft_output.resize(fft_size);
}

FFTProcessor::~FFTProcessor() {
}

void FFTProcessor::setFFTSize(size_t new_size) {
    fft_size = new_size;
    fft_input.resize(fft_size);
    fft_output.resize(fft_size);
}

void FFTProcessor::process(const std::vector<uint8_t>& iq_samples, std::vector<float>& magnitude) {
    // Convert IQ samples (uint8) to complex float
    size_t sample_count = std::min(iq_samples.size() / 2, fft_size);

    for (size_t i = 0; i < sample_count; i++) {
        // RTL-SDR outputs unsigned 8-bit IQ, centered at 127
        float i_val = (iq_samples[i * 2] - 127.0f) / 128.0f;
        float q_val = (iq_samples[i * 2 + 1] - 127.0f) / 128.0f;
        fft_input[i] = std::complex<float>(i_val, q_val);
    }

    // Apply window function
    applyWindow(fft_input);

    // Compute FFT
    computeFFT(fft_input, fft_output);

    // Calculate magnitude spectrum
    magnitude.resize(fft_size);
    for (size_t i = 0; i < fft_size; i++) {
        float real = fft_output[i].real();
        float imag = fft_output[i].imag();
        float mag = std::sqrt(real * real + imag * imag);

        // Convert to dB scale
        magnitude[i] = 20.0f * std::log10(mag + 1e-10f);
    }

    // FFT shift - move DC to center
    std::rotate(magnitude.begin(), magnitude.begin() + fft_size / 2, magnitude.end());
}

void FFTProcessor::applyWindow(std::vector<std::complex<float>>& data) {
    // Hann window
    for (size_t i = 0; i < data.size(); i++) {
        float window = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (data.size() - 1)));
        data[i] *= window;
    }
}

void FFTProcessor::computeFFT(const std::vector<std::complex<float>>& input,
                               std::vector<std::complex<float>>& output) {
    // Simple Cooley-Tukey FFT implementation
    // For production, consider using FFTW library for better performance

    size_t n = input.size();

    // Base case
    if (n <= 1) {
        output = input;
        return;
    }

    // Divide
    std::vector<std::complex<float>> even(n / 2);
    std::vector<std::complex<float>> odd(n / 2);

    for (size_t i = 0; i < n / 2; i++) {
        even[i] = input[i * 2];
        odd[i] = input[i * 2 + 1];
    }

    // Conquer
    std::vector<std::complex<float>> even_fft(n / 2);
    std::vector<std::complex<float>> odd_fft(n / 2);

    computeFFT(even, even_fft);
    computeFFT(odd, odd_fft);

    // Combine
    output.resize(n);
    for (size_t k = 0; k < n / 2; k++) {
        std::complex<float> t = std::polar(1.0f, -2.0f * (float)M_PI * k / n) * odd_fft[k];
        output[k] = even_fft[k] + t;
        output[k + n / 2] = even_fft[k] - t;
    }
}
