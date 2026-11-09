#ifndef AUDIO_DEMODULATOR_H
#define AUDIO_DEMODULATOR_H

#include <vector>
#include <complex>
#include <cstdint>

class AudioDemodulator {
public:
    enum DemodMode {
        MODE_FM,   // Wideband FM (broadcast)
        MODE_NFM,  // Narrowband FM
        MODE_AM,   // Amplitude Modulation
        MODE_USB,  // Upper Sideband
        MODE_LSB   // Lower Sideband
    };

    AudioDemodulator(uint32_t sdr_sample_rate, uint32_t audio_sample_rate = 48000);
    ~AudioDemodulator();

    // Demodulate IQ samples and output audio
    void demodulate(const std::vector<uint8_t>& iq_samples,
                    std::vector<int16_t>& audio_out);

    void setMode(DemodMode mode) { current_mode = mode; }
    DemodMode getMode() const { return current_mode; }

    // Update the input sample rate (when the SDR rate changes at runtime)
    void setSampleRate(uint32_t rate);

    void setDemodBandwidth(uint32_t bandwidth_hz) { demod_bandwidth = bandwidth_hz; }
    uint32_t getDemodBandwidth() const { return demod_bandwidth; }

    void setVolume(float volume);
    float getVolume() const { return volume; }

    void setSquelch(float threshold);
    float getSquelch() const { return squelch_threshold; }

    void setDeemphasis(bool enable) { deemphasis_enabled = enable; }
    bool getDeemphasis() const { return deemphasis_enabled; }

    // Set deemphasis time constant in microseconds (50 = EU/Asia, 75 = US)
    void setDeemphasisTau(float tau_us);
    float getDeemphasisTau() const { return deemphasis_tau_us_; }

    void setAudioAGC(bool enable) { agc_enabled = enable; }
    bool getAudioAGC() const { return agc_enabled; }

    bool isSquelchOpen() const { return squelch_open; }

private:
    DemodMode current_mode;
    uint32_t sdr_sample_rate;
    uint32_t audio_sample_rate;
    float volume;
    float squelch_threshold;        // Open threshold (power)
    float squelch_close_threshold_; // Close threshold (hysteresis: open * 0.7)
    bool deemphasis_enabled;
    bool squelch_open;
    uint32_t demod_bandwidth;  // Current demodulation bandwidth in Hz

    // Audio AGC state (time-based attack/release, independent of chunk size)
    float agc_gain;
    float agc_target;
    float agc_attack_s_;
    float agc_release_s_;
    bool agc_enabled;

    // Previous sample for FM demodulation
    std::complex<float> prev_sample;
    bool fm_has_prev_;

    // Deemphasis filter state (for FM)
    float deemph_state;
    float deemph_alpha;
    float deemphasis_tau_us_;   // 50 (EU/Asia) or 75 (US)

    // Streaming DSP state carried across demodulation chunks. Resetting
    // these per chunk produces audible clicks at the chunk rate.
    float lpf_state_[2][4];     // per biquad: x1, x2, y1, y2
    double resamp_pos_;         // fractional input position for next output
    float resamp_last_;         // last input sample of previous chunk
    float dc_prev_in_;
    float dc_prev_out_;
    float dc_r_;
    void removeDC(std::vector<float>& audio);

    // Pre-allocated working buffers (avoid per-frame allocations)
    std::vector<std::complex<float>> iq_buffer_;
    std::vector<float> audio_buffer_;

    // Convert uint8 IQ to complex float, with DC removal and power accumulation in single pass
    // Returns the mean signal power (for squelch)
    float iqToComplexWithPower(const std::vector<uint8_t>& iq_samples,
                               std::vector<std::complex<float>>& complex_out);

    // FM demodulation (phase discriminator)
    void demodulateFM(const std::vector<std::complex<float>>& iq,
                      std::vector<float>& audio);

    // AM demodulation (envelope detection)
    void demodulateAM(const std::vector<std::complex<float>>& iq,
                      std::vector<float>& audio);

    // SSB demodulation (single sideband)
    void demodulateSSB(const std::vector<std::complex<float>>& iq,
                       std::vector<float>& audio,
                       bool upper_sideband);

    // Audio resampling (SDR rate -> audio rate)
    void resample(const std::vector<float>& input,
                  std::vector<int16_t>& output);

    // Apply low-pass filter for anti-aliasing (before resampling)
    void applyLowPassFilter(std::vector<float>& audio, float cutoff_hz);

    // Apply audio AGC (automatic gain control)
    void applyAudioAGC(std::vector<float>& audio);

    // Apply FM deemphasis filter on int16 samples (after resampling)
    void applyDeemphasisInt16(std::vector<int16_t>& audio);
};

#endif // AUDIO_DEMODULATOR_H
