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
        MODE_AM    // Amplitude Modulation
    };

    AudioDemodulator(uint32_t sdr_sample_rate, uint32_t audio_sample_rate = 48000);
    ~AudioDemodulator();

    // Demodulate IQ samples and output audio
    void demodulate(const std::vector<uint8_t>& iq_samples,
                    std::vector<int16_t>& audio_out);

    void setMode(DemodMode mode) { current_mode = mode; }
    DemodMode getMode() const { return current_mode; }

    void setDemodBandwidth(uint32_t bandwidth_hz) { demod_bandwidth = bandwidth_hz; }
    uint32_t getDemodBandwidth() const { return demod_bandwidth; }

    void setVolume(float volume);
    float getVolume() const { return volume; }

    void setSquelch(float threshold);
    float getSquelch() const { return squelch_threshold; }

    void setDeemphasis(bool enable) { deemphasis_enabled = enable; }
    bool getDeemphasis() const { return deemphasis_enabled; }

    void setAudioAGC(bool enable) { agc_enabled = enable; }
    bool getAudioAGC() const { return agc_enabled; }

    bool isSquelchOpen() const { return squelch_open; }

private:
    DemodMode current_mode;
    uint32_t sdr_sample_rate;
    uint32_t audio_sample_rate;
    float volume;
    float squelch_threshold;
    bool deemphasis_enabled;
    bool squelch_open;
    uint32_t demod_bandwidth;  // Current demodulation bandwidth in Hz

    // Audio AGC state
    float agc_gain;
    float agc_target;
    float agc_attack;
    float agc_decay;
    bool agc_enabled;

    // Previous sample for FM demodulation
    std::complex<float> prev_sample;

    // Deemphasis filter state (for FM)
    float deemph_state;
    float deemph_alpha;

    // Low-pass filter state (for noise reduction)
    std::vector<float> lpf_buffer;
    size_t lpf_index;

    // Convert uint8 IQ to complex float
    void iqToComplex(const std::vector<uint8_t>& iq_samples,
                     std::vector<std::complex<float>>& complex_out);

    // FM demodulation (phase discriminator)
    void demodulateFM(const std::vector<std::complex<float>>& iq,
                      std::vector<float>& audio);

    // AM demodulation (envelope detection)
    void demodulateAM(const std::vector<std::complex<float>>& iq,
                      std::vector<float>& audio);

    // Audio resampling (SDR rate -> audio rate)
    void resample(const std::vector<float>& input,
                  std::vector<int16_t>& output);

    // Apply low-pass filter for anti-aliasing (before resampling)
    void applyLowPassFilter(std::vector<float>& audio, float cutoff_hz);

    // Apply audio AGC (automatic gain control)
    void applyAudioAGC(std::vector<float>& audio);

    // Apply noise gate to suppress low-level noise
    void applyNoiseGate(std::vector<float>& audio, float threshold);

    // Apply FM deemphasis filter (50us/75us) on float samples
    void applyDeemphasis(std::vector<float>& audio);

    // Apply FM deemphasis filter on int16 samples (after resampling)
    void applyDeemphasisInt16(std::vector<int16_t>& audio);

    // Check squelch (signal strength detection)
    bool checkSquelch(const std::vector<std::complex<float>>& iq);
};

#endif // AUDIO_DEMODULATOR_H
