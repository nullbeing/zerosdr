#include "audio_demodulator.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <iostream>

AudioDemodulator::AudioDemodulator(uint32_t sdr_sample_rate, uint32_t audio_sample_rate)
    : current_mode(MODE_FM),
      sdr_sample_rate(sdr_sample_rate),
      audio_sample_rate(audio_sample_rate),
      volume(0.5f),
      squelch_threshold(0.01f),
      squelch_close_threshold_(0.007f), // 0.01 * 0.7 (hysteresis)
      deemphasis_enabled(true),
      squelch_open(false),
      demod_bandwidth(200000),  // Default: 200 kHz for FM
      agc_gain(1.0f),
      agc_target(0.40f),     // Target RMS level (~-8 dBFS)
      agc_attack_s_(0.05f),  // 50 ms attack for loud signals
      agc_release_s_(0.50f), // 500 ms release for weak signals
      agc_enabled(true),
      prev_sample(0.0f, 0.0f),
      fm_has_prev_(false),
      deemph_state(0.0f),
      deemphasis_tau_us_(50.0f),
      resamp_pos_(0.0),
      resamp_last_(0.0f),
      dc_prev_in_(0.0f),
      dc_prev_out_(0.0f),
      dc_r_(0.0f) {

    // Initialize deemphasis filter (default 50us: CN/EU/Asia FM broadcast)
    deemph_alpha = 1.0f / (1.0f + audio_sample_rate * (deemphasis_tau_us_ * 1e-6f));

    // Streaming high-pass used to remove DC without per-chunk steps.
    // ~5 Hz corner, independent of the SDR sample rate.
    dc_r_ = std::exp(-2.0f * (float)M_PI * 5.0f / (float)sdr_sample_rate);

    std::memset(lpf_state_, 0, sizeof(lpf_state_));
}

AudioDemodulator::~AudioDemodulator() {
}

void AudioDemodulator::setVolume(float vol) {
    volume = std::clamp(vol, 0.0f, 1.0f);
}

void AudioDemodulator::setSquelch(float threshold) {
    // 'threshold' is the user-facing fraction of full-scale amplitude.
    // The internal comparison is against mean(|iq|²) (power domain),
    // so square the input. This makes "5%" mean "5% amplitude" which is
    // the user's intuitive mental model.
    float amp = std::clamp(threshold, 0.0f, 1.0f);
    squelch_threshold = amp * amp;
    // Hysteresis: close threshold = 70% of open threshold (in power)
    // Prevents chattering when signal hovers around the open threshold
    squelch_close_threshold_ = squelch_threshold * 0.7f;
}

void AudioDemodulator::setSampleRate(uint32_t rate) {
    if (rate == 0 || rate == sdr_sample_rate) {
        return;
    }
    sdr_sample_rate = rate;
    dc_r_ = std::exp(-2.0f * (float)M_PI * 5.0f / (float)sdr_sample_rate);
    resamp_pos_ = 0.0;
    resamp_last_ = 0.0f;
    fm_has_prev_ = false;
    dc_prev_in_ = 0.0f;
    dc_prev_out_ = 0.0f;
    std::memset(lpf_state_, 0, sizeof(lpf_state_));
}

void AudioDemodulator::setDeemphasisTau(float tau_us) {
    // Accept only the two broadcast standards; reject anything weird
    if (tau_us < 60.0f) {
        deemphasis_tau_us_ = 50.0f;
    } else {
        deemphasis_tau_us_ = 75.0f;
    }
    // Recompute filter coefficient for the new time constant
    deemph_alpha = 1.0f / (1.0f + audio_sample_rate * (deemphasis_tau_us_ * 1e-6f));
}

void AudioDemodulator::demodulate(const std::vector<uint8_t>& iq_samples,
                                   std::vector<int16_t>& audio_out) {
    if (iq_samples.empty()) {
        audio_out.clear();
        return;
    }

    // Convert IQ to complex and compute signal power in a single pass
    float power = iqToComplexWithPower(iq_samples, iq_buffer_);

    // Squelch with hysteresis: open at upper threshold, close at lower threshold
    // Prevents "chattering" when signal hovers around the open threshold
    if (squelch_threshold <= 0.0f) {
        squelch_open = true;
    } else if (squelch_open) {
        squelch_open = (power >= squelch_close_threshold_);
    } else {
        squelch_open = (power >= squelch_threshold);
    }
    if (!squelch_open) {
        size_t output_size = iq_buffer_.size() * audio_sample_rate / sdr_sample_rate;
        audio_out.assign(output_size, 0);
        // Drop stale streaming state so the next squelch opening starts
        // cleanly instead of resuming an old phase.
        fm_has_prev_ = false;
        resamp_pos_ = 0.0;
        resamp_last_ = 0.0f;
        dc_prev_in_ = 0.0f;
        dc_prev_out_ = 0.0f;
        std::memset(lpf_state_, 0, sizeof(lpf_state_));
        return;
    }

    // Demodulate based on mode (writes into pre-allocated audio_buffer_)
    switch (current_mode) {
        case MODE_FM:
        case MODE_NFM:
            demodulateFM(iq_buffer_, audio_buffer_);
            break;
        case MODE_AM:
            demodulateAM(iq_buffer_, audio_buffer_);
            break;
        case MODE_USB:
            demodulateSSB(iq_buffer_, audio_buffer_, true);
            break;
        case MODE_LSB:
            demodulateSSB(iq_buffer_, audio_buffer_, false);
            break;
    }

    // Anti-aliasing / audio bandwidth filter before resampling.
    // SSB's RF bandwidth is also its audio bandwidth. AM/NFM carry two
    // sidebands, so their audio cutoff is half the RF bandwidth.
    float cutoff;
    if (current_mode == MODE_USB || current_mode == MODE_LSB) {
        cutoff = (float)demod_bandwidth;
    } else {
        cutoff = demod_bandwidth / 2.0f;
    }
    if (cutoff > 15000.0f) cutoff = 15000.0f;
    if (cutoff < 100.0f) cutoff = 100.0f;
    applyLowPassFilter(audio_buffer_, cutoff);

    // AGC after filtering so it reacts to the audio band, not out-of-band
    // noise. It uses time-based attack/release, not a per-chunk multiplier.
    applyAudioAGC(audio_buffer_);

    // Resample to audio rate
    resample(audio_buffer_, audio_out);

    // Apply deemphasis for FM modes (after resampling)
    if ((current_mode == MODE_FM || current_mode == MODE_NFM) && deemphasis_enabled) {
        applyDeemphasisInt16(audio_out);
    }
}

float AudioDemodulator::iqToComplexWithPower(const std::vector<uint8_t>& iq_samples,
                                              std::vector<std::complex<float>>& complex_out) {
    size_t sample_count = iq_samples.size() / 2;
    complex_out.resize(sample_count);

    // Single pass: convert to complex and accumulate for DC offset
    std::complex<float> dc_sum(0.0f, 0.0f);
    for (size_t i = 0; i < sample_count; i++) {
        float i_val = (iq_samples[i * 2] - 127.0f) / 128.0f;
        float q_val = (iq_samples[i * 2 + 1] - 127.0f) / 128.0f;
        complex_out[i] = std::complex<float>(i_val, q_val);
        dc_sum += complex_out[i];
    }
    std::complex<float> dc_offset = dc_sum / (float)sample_count;

    // Second pass: subtract DC and accumulate power (signal mean)
    float power = 0.0f;
    for (auto& sample : complex_out) {
        sample -= dc_offset;
        power += std::norm(sample);
    }
    return power / (float)sample_count;
}

void AudioDemodulator::demodulateFM(const std::vector<std::complex<float>>& iq,
                                     std::vector<float>& audio) {
    audio.resize(iq.size());
    if (iq.empty()) return;

    size_t start = 0;
    if (!fm_has_prev_) {
        // First sample after startup has no valid previous phase.
        prev_sample = iq[0];
        audio[0] = 0.0f;
        fm_has_prev_ = true;
        start = 1;
    }

    for (size_t i = start; i < iq.size(); i++) {
        // Phase discriminator
        std::complex<float> product = iq[i] * std::conj(prev_sample);

        float magnitude = std::abs(product);
        if (magnitude < 1e-10f) {
            audio[i] = 0.0f;
        } else {
            audio[i] = std::arg(product) / M_PI;
        }

        prev_sample = iq[i];
    }

    // Streaming DC removal; do not subtract a per-chunk mean.
    removeDC(audio);

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

    // Streaming DC removal; the audio AGC handles level, so a per-chunk
    // max normalization (which caused level jumps) is not used here.
    removeDC(audio);

    for (auto& sample : audio) {
        sample = std::clamp(sample * 2.0f, -1.0f, 1.0f);
    }
}

void AudioDemodulator::demodulateSSB(const std::vector<std::complex<float>>& iq,
                                      std::vector<float>& audio,
                                      bool upper_sideband) {
    audio.resize(iq.size());

    for (size_t i = 0; i < iq.size(); i++) {
        std::complex<float> sample = iq[i];

        // For LSB, conjugate the signal (flips spectrum)
        if (!upper_sideband) {
            sample = std::conj(sample);
        }

        // Extract real part (this is the demodulated audio)
        audio[i] = sample.real();
    }

    // Streaming DC removal
    removeDC(audio);

    // Apply gain (SSB typically needs more gain than AM)
    float gain = 3.0f;
    for (auto& sample : audio) {
        sample = std::clamp(sample * gain, -1.0f, 1.0f);
    }
}

void AudioDemodulator::resample(const std::vector<float>& input,
                                 std::vector<int16_t>& output) {
    output.clear();
    if (input.empty()) {
        return;
    }

    const double ratio = (double)sdr_sample_rate / (double)audio_sample_rate;
    const double n = (double)input.size();

    if (input.size() < 2) {
        resamp_last_ = input.back();
        return;
    }

    // Streaming linear interpolation. `resamp_pos_` carries the fractional
    // position across chunks, so output samples are continuous rather than
    // restarting at input sample 0 for every 20 ms chunk.
    output.reserve((size_t)(n / ratio) + 2);
    double p = resamp_pos_;

    while (p < n - 1.0) {
        int idx = (int)std::floor(p);
        float frac = (float)(p - (double)idx);

        float x0 = (idx < 0) ? resamp_last_ : input[idx];
        float x1 = input[idx + 1];
        float sum = x0 + frac * (x1 - x0);

        sum *= volume;
        sum = std::clamp(sum, -1.0f, 1.0f);
        output.push_back((int16_t)(sum * 32767.0f));

        p += ratio;
    }

    resamp_pos_ = p - n;
    resamp_last_ = input.back();
}

void AudioDemodulator::removeDC(std::vector<float>& audio) {
    for (auto& sample : audio) {
        float out = sample - dc_prev_in_ + dc_r_ * dc_prev_out_;
        dc_prev_in_ = sample;
        dc_prev_out_ = out;
        sample = out;
    }
}

void AudioDemodulator::applyLowPassFilter(std::vector<float>& audio, float cutoff_hz) {
    if (audio.empty()) return;

    float ratio = cutoff_hz / (float)sdr_sample_rate;
    if (ratio <= 0.0f || ratio >= 0.5f) return;  // invalid cutoff, skip

    // 4th order Butterworth = 2 cascaded biquads with these Q values
    const float Q1 = 0.54119610f;  // 1/(2*cos(pi/8))
    const float Q2 = 1.30656296f;  // 1/(2*cos(3pi/8))

    // Pre-warp and compute biquad coefficients (standard RBJ cookbook form)
    float omega = 2.0f * M_PI * ratio;
    float sn = std::sin(omega);
    float cs = std::cos(omega);

    // Two biquad coefficient sets, computed inline to avoid struct overhead
    float coeffs[2][5];  // [bq1/bq2][b0, b1, b2, a1, a2]
    float Qs[2] = {Q1, Q2};
    for (int s = 0; s < 2; s++) {
        float alpha = sn / (2.0f * Qs[s]);
        float b0 = (1.0f - cs) * 0.5f;
        float b1 = 1.0f - cs;
        float b2 = (1.0f - cs) * 0.5f;
        float a0 = 1.0f + alpha;
        float a1 = -2.0f * cs;
        float a2 = 1.0f - alpha;
        coeffs[s][0] = b0 / a0;
        coeffs[s][1] = b1 / a0;
        coeffs[s][2] = b2 / a0;
        coeffs[s][3] = a1 / a0;
        coeffs[s][4] = a2 / a0;
    }

    // Causal 4th-order filter. The biquad state is kept across calls so
    // there is no transient at each 20 ms audio chunk boundary.
    // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    for (int s = 0; s < 2; s++) {
        const float b0 = coeffs[s][0], b1 = coeffs[s][1], b2 = coeffs[s][2];
        const float a1 = coeffs[s][3], a2 = coeffs[s][4];
        float x1 = lpf_state_[s][0];
        float x2 = lpf_state_[s][1];
        float y1 = lpf_state_[s][2];
        float y2 = lpf_state_[s][3];

        for (auto& sample : audio) {
            float x0 = sample;
            float y0 = b0*x0 + b1*x1 + b2*x2 - a1*y1 - a2*y2;
            x2 = x1; x1 = x0;
            y2 = y1; y1 = y0;
            sample = y0;
        }

        lpf_state_[s][0] = x1;
        lpf_state_[s][1] = x2;
        lpf_state_[s][2] = y1;
        lpf_state_[s][3] = y2;
    }
}

void AudioDemodulator::applyAudioAGC(std::vector<float>& audio) {
    if (!agc_enabled || audio.empty()) return;

    // Calculate RMS level.
    float rms = 0.0f;
    for (const auto& sample : audio) {
        rms += sample * sample;
    }
    rms = std::sqrt(rms / audio.size());

    // Don't let silence/noise slowly run the gain to its maximum.
    if (rms < 1e-4f) {
        return;
    }

    // Time-based smoothing. The old code multiplied the gain once per
    // chunk, so its effective time constant depended on the chunk length.
    float dt = (float)audio.size() / (float)sdr_sample_rate;
    float desired = agc_target / rms;

    // Limit the boost/cut range. Excessive gain boosts noise and makes the
    // signal sound compressed.
    desired = std::clamp(desired, 0.2f, 12.0f);

    float coeff;
    if (desired < agc_gain) {
        // Attack: reduce gain for loud signals.
        coeff = 1.0f - std::exp(-dt / agc_attack_s_);
    } else {
        // Release: increase gain slowly for weaker signals.
        coeff = 1.0f - std::exp(-dt / agc_release_s_);
    }
    float gain_start = agc_gain;
    agc_gain += (desired - agc_gain) * coeff;
    agc_gain = std::clamp(agc_gain, 0.2f, 12.0f);
    float gain_end = agc_gain;

    // Ramp the gain across the chunk instead of applying a constant value
    // and stepping at the chunk boundary.
    float step = (gain_end - gain_start) / (float)audio.size();
    float gain = gain_start;
    for (auto& sample : audio) {
        sample = std::clamp(sample * gain, -1.0f, 1.0f);
        gain += step;
    }
}

void AudioDemodulator::applyDeemphasisInt16(std::vector<int16_t>& audio) {
    for (size_t i = 0; i < audio.size(); i++) {
        float sample = audio[i] / 32767.0f;
        deemph_state = deemph_alpha * sample + (1.0f - deemph_alpha) * deemph_state;
        audio[i] = (int16_t)(std::clamp(deemph_state, -1.0f, 1.0f) * 32767.0f);
    }
}
