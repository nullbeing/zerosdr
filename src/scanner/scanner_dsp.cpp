#include "scanner/scanner_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace scanner {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Blackman-windowed sinc low-pass, unity DC gain. `cutoff` is the -6 dB
// point as a fraction of the sample rate.
std::vector<float> designLowpass(size_t taps, double cutoff) {
    std::vector<float> h(taps);
    const double mid = (static_cast<double>(taps) - 1.0) / 2.0;
    double sum = 0.0;
    for (size_t n = 0; n < taps; ++n) {
        const double x = static_cast<double>(n) - mid;
        const double sinc = (std::fabs(x) < 1e-9)
                                ? 2.0 * cutoff
                                : std::sin(2.0 * kPi * cutoff * x) / (kPi * x);
        const double w = 0.42 -
                         0.5 * std::cos(2.0 * kPi * n / (taps - 1)) +
                         0.08 * std::cos(4.0 * kPi * n / (taps - 1));
        h[n] = static_cast<float>(sinc * w);
        sum += h[n];
    }
    for (float& v : h) v = static_cast<float>(v / sum);
    return h;
}

float percentile(std::vector<float> values, float fraction) {
    if (values.empty()) return -200.0f;
    size_t idx = static_cast<size_t>(fraction * (values.size() - 1));
    idx = std::min(idx, values.size() - 1);
    std::nth_element(values.begin(), values.begin() + idx, values.end());
    return values[idx];
}

}  // namespace

const char* modeName(Mode mode) {
    switch (mode) {
        case Mode::AM: return "AM";
        case Mode::NFM: return "NFM";
    }
    return "?";
}

float fftBinPower(float fft_value) {
    // FFTProcessor: value = 20*log10(|X|^2).
    return std::pow(10.0f, fft_value / 20.0f);
}

float powerToDb(float power) {
    return 10.0f * std::log10(std::max(power, 1e-20f));
}

bool isCrystalHarmonic(uint32_t hz, uint32_t half_width_hz) {
    if (hz < kCrystalHz / 2) return false;
    const uint32_t r = hz % kCrystalHz;
    return r <= half_width_hz || kCrystalHz - r <= half_width_hz;
}

uint32_t snapToRaster(double hz, uint32_t raster_hz) {
    if (hz <= 0.0) return 0;
    if (raster_hz == 0) return static_cast<uint32_t>(std::llround(hz));
    const double steps = std::round(hz / static_cast<double>(raster_hz));
    return static_cast<uint32_t>(std::llround(steps * raster_hz));
}

// ---------------------------------------------------------------------------
// SpectrumAverager

void SpectrumAverager::reset(size_t bins) {
    sum_.assign(bins, 0.0f);
    frames_ = 0;
}

void SpectrumAverager::add(const std::vector<float>& fft_frame) {
    if (sum_.size() != fft_frame.size()) reset(fft_frame.size());
    for (size_t i = 0; i < fft_frame.size(); ++i) {
        sum_[i] += fftBinPower(fft_frame[i]);
    }
    ++frames_;
}

bool SpectrumAverager::result(std::vector<float>& db_out) const {
    if (frames_ == 0 || sum_.empty()) return false;
    const size_t n = sum_.size();
    const float inv = 1.0f / static_cast<float>(frames_);
    db_out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float left = sum_[i > 0 ? i - 1 : 0];
        const float right = sum_[i + 1 < n ? i + 1 : n - 1];
        db_out[i] = powerToDb((left + 2.0f * sum_[i] + right) * 0.25f * inv);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Detection

SliceResult detectSignals(const std::vector<float>& db, const SliceGeometry& geo,
                          float threshold_db, uint32_t raster_hz) {
    SliceResult result;
    const size_t last = std::min(geo.last_bin, db.size());
    if (geo.first_bin + 3 >= last) return result;
    const size_t n = last - geo.first_bin;

    std::vector<float> v(db.begin() + geo.first_bin, db.begin() + last);

    // Blank the LO leakage around DC with the quieter neighbour outside it.
    const long centre = static_cast<long>(geo.centre_bin);
    const long guard = geo.dc_guard_bins;
    const long lo_n = centre - guard - 1;
    const long hi_n = centre + guard + 1;
    float fill = 1e9f;
    if (lo_n >= 0 && lo_n < static_cast<long>(db.size())) fill = std::min(fill, db[lo_n]);
    if (hi_n >= 0 && hi_n < static_cast<long>(db.size())) fill = std::min(fill, db[hi_n]);
    if (fill < 1e8f) {
        for (long b = centre - guard; b <= centre + guard; ++b) {
            const long i = b - static_cast<long>(geo.first_bin);
            if (i >= 0 && i < static_cast<long>(n)) v[static_cast<size_t>(i)] = fill;
        }
    }

    result.floor_db = percentile(v, 0.2f);
    const float open = result.floor_db + threshold_db;
    // Hysteresis keeps one modulated signal from splitting into two hits.
    const float hold = open - std::min(3.0f, threshold_db * 0.5f);

    size_t i = 0;
    while (i < n) {
        if (v[i] < open) {
            ++i;
            continue;
        }
        size_t peak = i;
        size_t j = i;
        while (j < n && v[j] >= hold) {
            if (v[j] > v[peak]) peak = j;
            ++j;
        }

        double delta = 0.0;
        if (peak > 0 && peak + 1 < n) {
            const double a = v[peak - 1];
            const double b = v[peak];
            const double c = v[peak + 1];
            const double denom = a - 2.0 * b + c;
            if (std::fabs(denom) > 1e-6) {
                delta = std::clamp(0.5 * (a - c) / denom, -0.5, 0.5);
            }
        }
        const double bin = static_cast<double>(geo.first_bin + peak) + delta;
        const double hz = geo.centre_hz +
                          (bin - static_cast<double>(geo.centre_bin)) * geo.bin_hz;
        Detection det;
        det.hz = snapToRaster(hz, raster_hz);
        det.level_db = v[peak];
        det.snr_db = v[peak] - result.floor_db;
        bool merged = false;
        for (Detection& other : result.hits) {
            if (other.hz == det.hz) {
                if (det.snr_db > other.snr_db) other = det;
                merged = true;
                break;
            }
        }
        if (!merged) result.hits.push_back(det);
        i = j;
    }

    // A direct-conversion tuner leaves a weak mirror of every strong signal
    // at 2*LO - f (FFTProcessor's IQ correction can leave one too). Drop a
    // hit that sits on the mirror of one at least 10 dB stronger. Done before
    // range clipping so an out-of-range carrier still cancels its image.
    const double tol = std::max<double>(raster_hz, 2.0 * geo.bin_hz) + geo.bin_hz;
    std::vector<bool> image(result.hits.size(), false);
    for (size_t a = 0; a < result.hits.size(); ++a) {
        const double mirror = 2.0 * geo.centre_hz - result.hits[a].hz;
        for (size_t b = 0; b < result.hits.size(); ++b) {
            if (a == b || result.hits[a].level_db < result.hits[b].level_db + 10.0f) continue;
            if (std::fabs(mirror - result.hits[b].hz) <= tol) image[b] = true;
        }
    }
    std::vector<Detection> kept;
    for (size_t k = 0; k < result.hits.size(); ++k) {
        const Detection& d = result.hits[k];
        if (!image[k] && d.hz >= geo.min_hz && d.hz <= geo.max_hz) kept.push_back(d);
    }
    result.hits.swap(kept);
    return result;
}

float channelPeakDb(const std::vector<float>& db, const SliceGeometry& geo,
                    double offset_hz, double half_width_hz) {
    if (db.empty() || geo.bin_hz <= 0.0) return -200.0f;
    const double centre = static_cast<double>(geo.centre_bin) + offset_hz / geo.bin_hz;
    const double half = half_width_hz / geo.bin_hz;
    long from = static_cast<long>(std::floor(centre - half));
    long to = static_cast<long>(std::ceil(centre + half));
    from = std::max<long>(from, static_cast<long>(geo.first_bin));
    to = std::min<long>(to, static_cast<long>(std::min(geo.last_bin, db.size())) - 1);
    float best = -200.0f;
    for (long b = from; b <= to; ++b) {
        if (std::labs(b - static_cast<long>(geo.centre_bin)) <= geo.dc_guard_bins) continue;
        best = std::max(best, db[static_cast<size_t>(b)]);
    }
    return best;
}

// ---------------------------------------------------------------------------
// Channelizer

Channelizer::Channelizer() {
    // -6 dB at 32 kHz: flat to ~10 kHz, >70 dB down from ~54 kHz, so
    // nothing folds into the +/-8 kHz channel after decimating to 64 kHz.
    decim_taps_ = designLowpass(128, 32000.0 / kInputRate);
    designChannelFilter();
    reset();
}

void Channelizer::designChannelFilter() {
    // AM: -6 dB at 5 kHz (8.33/25 kHz airband). NFM: 7.5 kHz (+/-5 kHz dev).
    const double cutoff = (mode_ == Mode::AM) ? 5000.0 : 7500.0;
    chan_taps_ = designLowpass(95, cutoff / kChannelRate);
}

void Channelizer::reset() {
    nco_phase_ = 0.0;
    decim_buf_.assign(decim_taps_.size() - 1, {0.0f, 0.0f});
    decim_next_ = decim_taps_.size() - 1;
    chan_hist_.assign(chan_taps_.size() - 1, {0.0f, 0.0f});
    fm_prev_ = {0.0f, 0.0f};
    dc_prev_in_ = 0.0f;
    dc_prev_out_ = 0.0f;
    lp_state_ = 0.0f;
    agc_env_ = 0.0f;
    resamp_pos_ = 0.0;
    resamp_last_ = 0.0f;
    audio_rms_ = 0.0f;
}

void Channelizer::setOffset(double offset_hz) { offset_hz_ = offset_hz; }

void Channelizer::setMode(Mode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    designChannelFilter();
    reset();
}

float Channelizer::process(const uint8_t* iq, size_t bytes,
                           std::vector<int16_t>& pcm) {
    pcm.clear();
    const size_t n = bytes / 2;
    if (n == 0) return -200.0f;

    // 1) Mix the wanted signal down to 0 Hz.
    const double w = -2.0 * kPi * offset_hz_ / kInputRate;
    std::complex<float> rot(static_cast<float>(std::cos(nco_phase_)),
                            static_cast<float>(std::sin(nco_phase_)));
    const std::complex<float> step(static_cast<float>(std::cos(w)),
                                   static_cast<float>(std::sin(w)));
    decim_buf_.reserve(decim_buf_.size() + n);
    for (size_t i = 0; i < n; ++i) {
        const std::complex<float> x((iq[2 * i] - 127.5f) / 128.0f,
                                    (iq[2 * i + 1] - 127.5f) / 128.0f);
        decim_buf_.push_back(x * rot);
        rot *= step;
        if ((i & 255) == 255) rot /= std::abs(rot);
    }
    nco_phase_ = std::fmod(nco_phase_ + w * static_cast<double>(n), 2.0 * kPi);

    // 2) Low-pass and keep every 16th sample.
    const size_t taps = decim_taps_.size();
    baseband_.clear();
    size_t pos = decim_next_;
    while (pos < decim_buf_.size()) {
        const std::complex<float>* x = &decim_buf_[pos + 1 - taps];
        float re = 0.0f;
        float im = 0.0f;
        for (size_t t = 0; t < taps; ++t) {
            re += x[t].real() * decim_taps_[t];
            im += x[t].imag() * decim_taps_[t];
        }
        baseband_.emplace_back(re, im);
        pos += kDecimation;
    }
    const size_t keep_from = decim_buf_.size() - (taps - 1);
    decim_next_ = pos - keep_from;
    decim_buf_.erase(decim_buf_.begin(), decim_buf_.begin() + keep_from);

    // 3) Channel filter, demodulate, condition the audio.
    const size_t ctaps = chan_taps_.size();
    const float dc_r = std::exp(-2.0f * static_cast<float>(kPi) * 30.0f / kChannelRate);
    const float lp_fc = (mode_ == Mode::AM) ? 4000.0f : 3200.0f;
    const float lp_a = 1.0f - std::exp(-2.0f * static_cast<float>(kPi) * lp_fc / kChannelRate);
    const float att = 1.0f - std::exp(-1.0f / (0.005f * kChannelRate));
    const float rel = 1.0f - std::exp(-1.0f / (0.300f * kChannelRate));
    const float max_gain = (mode_ == Mode::AM) ? 400.0f : 6.0f;
    const float fm_scale = static_cast<float>(kChannelRate / (2.0 * kPi * 5000.0));

    chan_hist_.insert(chan_hist_.end(), baseband_.begin(), baseband_.end());
    audio64_.resize(baseband_.size());
    double power = 0.0;
    double sq = 0.0;
    for (size_t k = 0; k < baseband_.size(); ++k) {
        const std::complex<float>* x = &chan_hist_[k];
        float re = 0.0f;
        float im = 0.0f;
        for (size_t t = 0; t < ctaps; ++t) {
            re += x[t].real() * chan_taps_[t];
            im += x[t].imag() * chan_taps_[t];
        }
        const std::complex<float> y(re, im);
        power += std::norm(y);

        float a;
        if (mode_ == Mode::AM) {
            a = std::abs(y);
        } else {
            const std::complex<float> d = y * std::conj(fm_prev_);
            fm_prev_ = y;
            a = (std::norm(d) > 1e-20f) ? std::arg(d) * fm_scale : 0.0f;
        }

        const float hp = a - dc_prev_in_ + dc_r * dc_prev_out_;
        dc_prev_in_ = a;
        dc_prev_out_ = hp;
        lp_state_ += lp_a * (hp - lp_state_);

        const float mag = std::fabs(lp_state_);
        agc_env_ += (mag > agc_env_ ? att : rel) * (mag - agc_env_);
        const float gain = std::min(max_gain, 0.35f / std::max(agc_env_, 1e-6f));
        const float out = lp_state_ * gain;
        sq += static_cast<double>(out) * out;
        audio64_[k] = std::clamp(out, -1.0f, 1.0f);
    }
    chan_hist_.erase(chan_hist_.begin(), chan_hist_.begin() + baseband_.size());

    if (!baseband_.empty()) {
        audio_rms_ = static_cast<float>(std::sqrt(sq / baseband_.size()));
    }

    // 4) 64 kHz -> 48 kHz, streaming linear interpolation.
    const double ratio = static_cast<double>(kChannelRate) / kAudioRate;
    const double len = static_cast<double>(audio64_.size());
    double p = resamp_pos_;
    while (p < len - 1.0) {
        const long idx = static_cast<long>(std::floor(p));
        const float frac = static_cast<float>(p - idx);
        const float x0 = idx < 0 ? resamp_last_ : audio64_[static_cast<size_t>(idx)];
        const float x1 = audio64_[static_cast<size_t>(idx + 1)];
        const float s = x0 + frac * (x1 - x0);
        pcm.push_back(static_cast<int16_t>(s * 30000.0f));
        p += ratio;
    }
    if (!audio64_.empty()) {
        resamp_pos_ = p - len;
        resamp_last_ = audio64_.back();
    }

    if (baseband_.empty()) return -200.0f;
    return powerToDb(static_cast<float>(power / baseband_.size()));
}

// ---------------------------------------------------------------------------
// SignalTable

bool TrackedSignal::heardInSecond(int64_t second) const {
    const int64_t age = activity_epoch - second;
    if (age < 0 || age >= 64) return false;
    return (activity >> age) & 1u;
}

bool SignalTable::near(uint32_t a, uint32_t b) const {
    const uint32_t d = a > b ? a - b : b - a;
    return d <= tolerance_hz_;
}

uint32_t SignalTable::observe(const Detection& det, double now) {
    const int64_t second = static_cast<int64_t>(std::floor(now));
    TrackedSignal* sig = nullptr;
    uint32_t best = 0xFFFFFFFFu;
    for (TrackedSignal& s : signals_) {
        const uint32_t d = s.hz > det.hz ? s.hz - det.hz : det.hz - s.hz;
        if (d <= tolerance_hz_ && d < best) {
            best = d;
            sig = &s;
        }
    }

    if (!sig) {
        TrackedSignal s;
        s.id = next_id_++;
        s.hz = det.hz;
        s.first_seen = now;
        s.burst_start = now;
        s.last_seen = now;
        s.activity_epoch = second;
        signals_.push_back(s);
        sig = &signals_.back();
    } else if (now - sig->last_seen > kBurstGap) {
        sig->burst_start = now;
    }

    sig->last_seen = std::max(sig->last_seen, now);
    sig->snr_db = sig->hits == 0 ? det.snr_db : 0.5f * sig->snr_db + 0.5f * det.snr_db;
    sig->level_db = det.level_db;
    ++sig->hits;

    const int64_t shift = second - sig->activity_epoch;
    if (shift > 0) {
        sig->activity = shift >= 64 ? 0 : (sig->activity << shift);
        sig->activity_epoch = second;
    }
    if (second <= sig->activity_epoch) {
        const int64_t age = sig->activity_epoch - second;
        if (age < 64) sig->activity |= (uint64_t{1} << age);
    }
    return sig->id;
}

void SignalTable::expire(double now, double keep_s) {
    signals_.erase(std::remove_if(signals_.begin(), signals_.end(),
                                  [&](const TrackedSignal& s) {
                                      return now - s.last_seen > keep_s;
                                  }),
                   signals_.end());
    snoozes_.erase(std::remove_if(snoozes_.begin(), snoozes_.end(),
                                  [&](const Snooze& s) { return s.until < now; }),
                   snoozes_.end());
}

void SignalTable::clear() {
    signals_.clear();
    snoozes_.clear();
}

const TrackedSignal* SignalTable::find(uint32_t hz) const {
    const TrackedSignal* found = nullptr;
    uint32_t best = 0xFFFFFFFFu;
    for (const TrackedSignal& s : signals_) {
        const uint32_t d = s.hz > hz ? s.hz - hz : hz - s.hz;
        if (d <= tolerance_hz_ && d < best) {
            best = d;
            found = &s;
        }
    }
    return found;
}

bool SignalTable::isLockedOut(uint32_t hz) const {
    for (uint32_t l : lockouts_) {
        if (near(l, hz)) return true;
    }
    return false;
}

bool SignalTable::toggleLockout(uint32_t hz) {
    for (auto it = lockouts_.begin(); it != lockouts_.end(); ++it) {
        if (near(*it, hz)) {
            lockouts_.erase(it);
            return false;
        }
    }
    lockouts_.push_back(hz);
    return true;
}

void SignalTable::snooze(uint32_t hz, double until) {
    for (Snooze& s : snoozes_) {
        if (near(s.hz, hz)) {
            s.until = std::max(s.until, until);
            return;
        }
    }
    snoozes_.push_back({hz, until});
}

bool SignalTable::isSnoozed(uint32_t hz, double now) const {
    for (const Snooze& s : snoozes_) {
        if (s.until >= now && near(s.hz, hz)) return true;
    }
    return false;
}

uint32_t SignalTable::pickNext(double since, double now, uint32_t exclude_hz,
                               uint32_t lo, uint32_t hi) const {
    const TrackedSignal* best = nullptr;
    for (const TrackedSignal& s : signals_) {
        if (s.last_seen < since) continue;
        if (s.hz < lo || s.hz > hi) continue;
        if (exclude_hz != 0 && near(s.hz, exclude_hz)) continue;
        if (isLockedOut(s.hz) || isSnoozed(s.hz, now)) continue;
        if (!best || s.snr_db > best->snr_db) best = &s;
    }
    return best ? best->hz : 0;
}

}  // namespace scanner
