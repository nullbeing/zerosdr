#ifndef SCANNER_DSP_H
#define SCANNER_DSP_H

// Hardware-free building blocks for the Scanner page: peak detection on an
// FFT slice, a narrow-band channelizer + AM/NFM demodulator, and the table of
// signals the scanner has heard. Kept free of rtl-sdr/ALSA so it can be unit
// tested on a desktop (tests/scanner_dsp_test.cpp).

#include <complex>
#include <cstdint>
#include <vector>

namespace scanner {

enum class Mode { AM = 0, NFM = 1 };
constexpr int kModeCount = 2;
const char* modeName(Mode mode);

// FFTProcessor reports 20*log10(|X|^2). Convert one bin to linear power.
float fftBinPower(float fft_value);
float powerToDb(float power);

// Average several FFTProcessor frames in linear power, then apply a [1 2 1]
// smoothing across bins. Output is in real dB (10*log10 power).
class SpectrumAverager {
public:
    void reset(size_t bins);
    void add(const std::vector<float>& fft_frame);
    // Returns false when no frame was added.
    bool result(std::vector<float>& db_out) const;
    int frames() const { return frames_; }

private:
    std::vector<float> sum_;
    int frames_ = 0;
};

struct Detection {
    uint32_t hz = 0;
    float level_db = 0.0f;
    float snr_db = 0.0f;
};

struct SliceResult {
    float floor_db = 0.0f;
    std::vector<Detection> hits;
};

struct SliceGeometry {
    size_t first_bin = 0;      // first usable bin (inclusive)
    size_t last_bin = 0;       // last usable bin (exclusive)
    size_t centre_bin = 0;     // bin that sits on the tuner LO (DC)
    double centre_hz = 0.0;    // tuner LO frequency
    double bin_hz = 1000.0;
    int dc_guard_bins = 3;     // bins either side of DC that are ignored
    uint32_t min_hz = 0;       // detections outside [min_hz, max_hz] are
    uint32_t max_hz = 0xFFFFFFFFu;  // dropped
};

// Find signals that stand `threshold_db` above the slice noise floor (20th
// percentile). Each contiguous region above the threshold yields one hit at
// its strongest bin, refined by parabolic interpolation and snapped to
// `raster_hz`. Mirror images (2*LO - f) of much stronger hits are dropped.
SliceResult detectSignals(const std::vector<float>& db, const SliceGeometry& geo,
                          float threshold_db, uint32_t raster_hz);

// Strongest level (dB) within +/- half_width_hz of `offset_hz` from the LO.
float channelPeakDb(const std::vector<float>& db, const SliceGeometry& geo,
                    double offset_hz, double half_width_hz);

uint32_t snapToRaster(double hz, uint32_t raster_hz);

// RTL-SDR dongles show a steady carrier on multiples of their 28.8 MHz
// reference crystal (144.000, 432.000 MHz, ...). Detections snapped onto one
// (every channel raster divides 28.8 MHz) are not real signals.
constexpr uint32_t kCrystalHz = 28800000;
bool isCrystalHarmonic(uint32_t hz, uint32_t half_width_hz = 2000);

// Narrow-band receiver: shifts a signal at `offset_hz` from the tuner LO to
// baseband, decimates 1.024 MS/s -> 64 kS/s, applies a channel filter and
// demodulates AM or NFM to 48 kHz audio. All filter state is carried across
// calls, so chunk boundaries are seamless.
class Channelizer {
public:
    static constexpr uint32_t kInputRate = 1024000;
    static constexpr uint32_t kDecimation = 16;
    static constexpr uint32_t kChannelRate = kInputRate / kDecimation;  // 64k
    static constexpr uint32_t kAudioRate = 48000;

    Channelizer();

    void reset();
    void setOffset(double offset_hz);
    void setMode(Mode mode);
    Mode mode() const { return mode_; }

    // Feed interleaved uint8 IQ. Appends 48 kHz audio to `pcm` (cleared
    // first). Returns the channel power after filtering, in dB.
    float process(const uint8_t* iq, size_t bytes, std::vector<int16_t>& pcm);

    // RMS of the last chunk of audio (0..1), before the output clamp.
    float audioRms() const { return audio_rms_; }

private:
    void designChannelFilter();

    Mode mode_ = Mode::AM;
    double offset_hz_ = 0.0;
    double nco_phase_ = 0.0;  // radians

    std::vector<float> decim_taps_;
    std::vector<std::complex<float>> decim_buf_;
    size_t decim_next_ = 0;

    std::vector<float> chan_taps_;
    std::vector<std::complex<float>> chan_hist_;

    std::complex<float> fm_prev_{0.0f, 0.0f};
    float dc_prev_in_ = 0.0f;
    float dc_prev_out_ = 0.0f;
    float lp_state_ = 0.0f;
    float agc_env_ = 0.0f;
    double resamp_pos_ = 0.0;
    float resamp_last_ = 0.0f;
    float audio_rms_ = 0.0f;

    std::vector<std::complex<float>> baseband_;
    std::vector<float> audio64_;
};

// A signal the scanner has heard.
struct TrackedSignal {
    uint32_t id = 0;
    uint32_t hz = 0;
    float snr_db = 0.0f;
    float level_db = 0.0f;
    double first_seen = 0.0;
    double last_seen = 0.0;
    double burst_start = 0.0;
    // Bit i set = heard during whole second (activity_epoch - i).
    uint64_t activity = 0;
    int64_t activity_epoch = 0;
    uint32_t hits = 0;

    double burstDuration() const { return last_seen - burst_start; }
    bool heardInSecond(int64_t second) const;
};

class SignalTable {
public:
    // Detections within `tolerance_hz` of a tracked signal update it.
    void setTolerance(uint32_t hz) { tolerance_hz_ = hz; }
    uint32_t tolerance() const { return tolerance_hz_; }

    // A gap longer than this starts a new burst (resets DUR).
    static constexpr double kBurstGap = 2.5;

    // Merge a detection. Returns the id of the tracked signal.
    uint32_t observe(const Detection& det, double now);
    // Drop signals not heard for `keep_s` seconds.
    void expire(double now, double keep_s);
    void clear();

    const std::vector<TrackedSignal>& signals() const { return signals_; }
    const TrackedSignal* find(uint32_t hz) const;

    // Lockouts are permanent skips (persisted by the page).
    bool isLockedOut(uint32_t hz) const;
    bool toggleLockout(uint32_t hz);  // returns true when now locked out
    void clearLockouts() { lockouts_.clear(); }
    const std::vector<uint32_t>& lockouts() const { return lockouts_; }
    void setLockouts(const std::vector<uint32_t>& hz) { lockouts_ = hz; }

    // Snoozes are temporary skips (Enter = skip this transmission).
    void snooze(uint32_t hz, double until);
    bool isSnoozed(uint32_t hz, double now) const;

    // Strongest signal heard at or after `since`, inside [lo, hi], that is
    // neither locked out, snoozed nor equal to `exclude_hz`.
    // Returns 0 when there is none.
    uint32_t pickNext(double since, double now, uint32_t exclude_hz,
                      uint32_t lo = 0, uint32_t hi = 0xFFFFFFFFu) const;

private:
    bool near(uint32_t a, uint32_t b) const;

    std::vector<TrackedSignal> signals_;
    std::vector<uint32_t> lockouts_;
    struct Snooze {
        uint32_t hz;
        double until;
    };
    std::vector<Snooze> snoozes_;
    uint32_t tolerance_hz_ = 5000;
    uint32_t next_id_ = 1;
};

}  // namespace scanner

#endif  // SCANNER_DSP_H
