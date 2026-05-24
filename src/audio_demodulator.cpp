#include "audio_demodulator.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iostream>

AudioDemodulator::AudioDemodulator(uint32_t sdr_sample_rate, uint32_t audio_sample_rate)
    : current_mode(MODE_FM),
      sdr_sample_rate(sdr_sample_rate),
      audio_sample_rate(audio_sample_rate),
      volume(0.5f),
      squelch_threshold(0.01f),
      deemphasis_enabled(true),
      squelch_open(false),
      demod_bandwidth(200000),  // Default: 200 kHz for FM
      agc_gain(1.0f),
      agc_target(0.3f),      // Target RMS level (30% of full scale)
      agc_attack(0.01f),     // Fast attack for loud signals
      agc_decay(0.001f),     // Slow decay for weak signals
      agc_enabled(true),
      prev_sample(0.0f, 0.0f),
      deemph_state(0.0f),
      lpf_index(0) {

    // Initialize deemphasis filter (75us time constant for US FM broadcast)
    float tau = 75e-6f;
    deemph_alpha = 1.0f / (1.0f + audio_sample_rate * tau);

    // Initialize low-pass filter buffer
    lpf_buffer.resize(5, 0.0f);
}

AudioDemodulator::~AudioDemodulator() {
}

void AudioDemodulator::setVolume(float vol) {
    volume = std::clamp(vol, 0.0f, 1.0f);
}

void AudioDemodulator::setSquelch(float threshold) {
    squelch_threshold = std::clamp(threshold, 0.0f, 1.0f);
}

void AudioDemodulator::demodulate(const std::vector<uint8_t>& iq_samples,
                                   std::vector<int16_t>& audio_out) {
    if (iq_samples.empty()) {
        audio_out.clear();
        return;
    }

    // Convert IQ samples to complex float
    std::vector<std::complex<float>> iq_complex;
    iqToComplex(iq_samples, iq_complex);

    // Check squelch
    squelch_open = checkSquelch(iq_complex);
    if (!squelch_open) {
        size_t output_size = iq_complex.size() * audio_sample_rate / sdr_sample_rate;
        audio_out.assign(output_size, 0);
        return;
    }

    // Demodulate based on mode
    std::vector<float> audio_float;
    switch (current_mode) {
        case MODE_FM:
        case MODE_NFM:
            demodulateFM(iq_complex, audio_float);
            break;
        case MODE_AM:
            demodulateAM(iq_complex, audio_float);
            break;
    }

    // Apply audio AGC (after demodulation, before filtering)
    applyAudioAGC(audio_float);

    // Apply low-pass filter BEFORE resampling (anti-aliasing)
    // Cutoff frequency based on demodulation bandwidth
    float cutoff = demod_bandwidth / 2.0f;  // Nyquist: half of bandwidth
    if (cutoff > 15000.0f) cutoff = 15000.0f;  // Cap at 15kHz for audio
    applyLowPassFilter(audio_float, cutoff);

    // Apply noise gate (threshold: 1% of full scale)
    applyNoiseGate(audio_float, 0.01f);

    // Resample to audio rate
    resample(audio_float, audio_out);

    // Apply deemphasis for FM modes (after resampling)
    if ((current_mode == MODE_FM || current_mode == MODE_NFM) && deemphasis_enabled) {
        applyDeemphasisInt16(audio_out);
    }
}

void AudioDemodulator::iqToComplex(const std::vector<uint8_t>& iq_samples,
                                    std::vector<std::complex<float>>& complex_out) {
    size_t sample_count = iq_samples.size() / 2;
    complex_out.resize(sample_count);

    // First pass: convert to complex
    for (size_t i = 0; i < sample_count; i++) {
        float i_val = (iq_samples[i * 2] - 127.0f) / 128.0f;
        float q_val = (iq_samples[i * 2 + 1] - 127.0f) / 128.0f;
        complex_out[i] = std::complex<float>(i_val, q_val);
    }

    // Remove DC offset (average of all samples)
    std::complex<float> dc_offset(0.0f, 0.0f);
    for (const auto& sample : complex_out) {
        dc_offset += sample;
    }
    dc_offset /= (float)sample_count;

    // Subtract DC offset
    for (auto& sample : complex_out) {
        sample -= dc_offset;
    }
}

void AudioDemodulator::demodulateFM(const std::vector<std::complex<float>>& iq,
                                     std::vector<float>& audio) {
    audio.resize(iq.size());

    for (size_t i = 0; i < iq.size(); i++) {
        // Phase discriminator
        std::complex<float> product = iq[i] * std::conj(prev_sample);

        // Avoid division by zero
        float magnitude = std::abs(product);
        if (magnitude < 1e-10f) {
            audio[i] = 0.0f;
        } else {
            // Normalized phase difference
            float phase_diff = std::arg(product) / M_PI;

            // Detect and suppress large phase jumps (discontinuities)
            // These are likely buffer boundary artifacts
            if (i == 0 && std::abs(phase_diff) > 0.5f) {
                // First sample: might be discontinuous from previous buffer
                audio[i] = 0.0f;  // Mute the glitch
            } else {
                audio[i] = phase_diff;
            }
        }

        prev_sample = iq[i];
    }

    // Remove DC offset from audio (important for FM)
    float dc = std::accumulate(audio.begin(), audio.end(), 0.0f) / audio.size();
    for (auto& sample : audio) {
        sample -= dc;
    }

    // Apply gain based on mode
    float gain = (current_mode == MODE_NFM) ? 3.0f : 1.5f;
    for (auto& sample : audio) {
        sample = std::clamp(sample * gain, -1.0f, 1.0f);
    }
}

void AudioDemodulator::demodulateAM(const std::vector<std::complex<float>>& iq,
                                     std::vector<float>& audio) {
    audio.resize(iq.size());

    for (size_t i = 0; i < iq.size(); i++) {
        audio[i] = std::abs(iq[i]);
    }

    // Remove DC
    float dc = std::accumulate(audio.begin(), audio.end(), 0.0f) / audio.size();
    for (auto& sample : audio) {
        sample -= dc;
    }

    // Normalize
    float max_val = *std::max_element(audio.begin(), audio.end(),
                                       [](float a, float b) { return std::abs(a) < std::abs(b); });
    if (max_val > 1e-6f) {
        for (auto& sample : audio) {
            sample = std::clamp(sample / max_val * 2.0f, -1.0f, 1.0f);
        }
    }
}

void AudioDemodulator::resample(const std::vector<float>& input,
                                 std::vector<int16_t>& output) {
    if (input.empty()) {
        output.clear();
        return;
    }

    float ratio = (float)sdr_sample_rate / audio_sample_rate;
    size_t output_size = (size_t)(input.size() / ratio);
    output.resize(output_size);

    for (size_t i = 0; i < output_size; i++) {
        float pos = i * ratio;
        size_t idx = (size_t)pos;

        if (idx + 1 >= input.size()) {
            output[i] = 0;
            continue;
        }

        float frac = pos - idx;
        float sample = input[idx] * (1.0f - frac) + input[idx + 1] * frac;

        // Apply volume
        sample *= volume;

        // Convert to int16
        sample = std::clamp(sample, -1.0f, 1.0f);
        output[i] = (int16_t)(sample * 32767.0f);
    }
}

void AudioDemodulator::applyLowPassFilter(std::vector<float>& audio, float cutoff_hz) {
    if (audio.empty()) return;

    // Two-stage bidirectional IIR filter (zero-phase)
    float dt = 1.0f / sdr_sample_rate;
    float rc = 1.0f / (2.0f * M_PI * cutoff_hz);
    float alpha = dt / (rc + dt);

    // First pass: forward
    float prev = audio[0];
    for (size_t i = 1; i < audio.size(); i++) {
        audio[i] = prev + alpha * (audio[i] - prev);
        prev = audio[i];
    }

    // Second pass: backward (zero-phase filtering)
    prev = audio[audio.size() - 1];
    for (int i = audio.size() - 2; i >= 0; i--) {
        audio[i] = prev + alpha * (audio[i] - prev);
        prev = audio[i];
    }
}

void AudioDemodulator::applyAudioAGC(std::vector<float>& audio) {
    if (!agc_enabled || audio.empty()) return;

    // Calculate RMS level
    float rms = 0.0f;
    for (const auto& sample : audio) {
        rms += sample * sample;
    }
    rms = std::sqrt(rms / audio.size());

    // Adjust gain
    if (rms > agc_target) {
        // Fast attack: reduce gain quickly for loud signals
        agc_gain *= (1.0f - agc_attack);
    } else if (rms < agc_target * 0.5f) {
        // Slow decay: increase gain slowly for weak signals
        agc_gain *= (1.0f + agc_decay);
    }

    // Clamp gain to reasonable range
    agc_gain = std::clamp(agc_gain, 0.5f, 20.0f);

    // Apply gain
    for (auto& sample : audio) {
        sample = std::clamp(sample * agc_gain, -1.0f, 1.0f);
    }
}

void AudioDemodulator::applyNoiseGate(std::vector<float>& audio, float threshold) {
    // Soft noise gate: attenuate samples below threshold
    for (auto& sample : audio) {
        float abs_val = std::abs(sample);
        if (abs_val < threshold) {
            // Quadratic attenuation curve for smooth transition
            float ratio = abs_val / threshold;
            sample *= ratio * ratio;
        }
    }
}

void AudioDemodulator::applyDeemphasis(std::vector<float>& audio) {
    for (size_t i = 0; i < audio.size(); i++) {
        deemph_state = deemph_alpha * audio[i] + (1.0f - deemph_alpha) * deemph_state;
        audio[i] = deemph_state;
    }
}

void AudioDemodulator::applyDeemphasisInt16(std::vector<int16_t>& audio) {
    for (size_t i = 0; i < audio.size(); i++) {
        float sample = audio[i] / 32767.0f;
        deemph_state = deemph_alpha * sample + (1.0f - deemph_alpha) * deemph_state;
        audio[i] = (int16_t)(std::clamp(deemph_state, -1.0f, 1.0f) * 32767.0f);
    }
}

bool AudioDemodulator::checkSquelch(const std::vector<std::complex<float>>& iq) {
    if (squelch_threshold <= 0.0f) {
        return true;
    }

    float power = 0.0f;
    for (const auto& sample : iq) {
        power += std::norm(sample);
    }
    power /= iq.size();

    return power > squelch_threshold;
}
