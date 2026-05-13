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

    size_t getFFTSize() const { return fft_size; }
    void setFFTSize(size_t new_size);

private:
    size_t fft_size;
    std::vector<std::complex<float>> fft_input;
    std::vector<std::complex<float>> fft_output;

    // Simple FFT implementation (can be replaced with FFTW for better performance)
    void computeFFT(const std::vector<std::complex<float>>& input,
                    std::vector<std::complex<float>>& output);
    void applyWindow(std::vector<std::complex<float>>& data);
};

#endif // FFT_PROCESSOR_H
