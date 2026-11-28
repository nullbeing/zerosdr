#include "pages/scanner_page.h"

#include "app_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <functional>
#include <fstream>
#include <iostream>
#include <unistd.h>

using scanner::Mode;

namespace {

constexpr float kRenderInterval = 1.0f / 20.0f;
constexpr int kSettleUs = 15000;
constexpr int kSweepFrames = 8;           // FFT frames averaged per slice
constexpr int kListenFrames = 6;          // FFT frames per 50 ms listen chunk
constexpr double kDropout = 0.4;          // carrier gap before HANG starts
constexpr double kKeepSeconds = 30.0;     // forget signals unheard this long
constexpr double kSkipSnooze = 15.0;      // ENTER skips a frequency this long
constexpr float kHysteresisDb = 3.0f;
constexpr uint32_t kMinHz = 24000000;
constexpr uint32_t kMaxHz = 1766000000;
constexpr uint32_t kMaxSpanHz = 300000000;
constexpr uint32_t kMinSpanHz = 25000;
// Channel steps offered in the range editor.
constexpr uint32_t kSteps[] = {2500, 5000, 6250, 10000, 12500, 25000};
constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

bool validRange(uint32_t start, uint32_t stop) {
    return start >= kMinHz && stop <= kMaxHz && stop > start + kMinSpanHz &&
           stop - start <= kMaxSpanHz;
}

std::string formatMhz(uint32_t hz) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.3f", hz / 1e6);
    return buf;
}

// 12500 -> "12.5", 6250 -> "6.25", 5000 -> "5".
std::string formatStepKhz(uint32_t hz) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.3f", hz / 1e3);
    std::string out = buf;
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

}  // namespace

const ScannerPage::Preset ScannerPage::kPresets[] = {
    {"AIR", 118000000, 137000000, Mode::AM, 5000},
    {"MARINE", 156000000, 162025000, Mode::NFM, 12500},
    {"2M HAM", 144000000, 148000000, Mode::NFM, 5000},
    {"70CM HAM", 430000000, 440000000, Mode::NFM, 5000},
    {"PMR446", 446000000, 446200000, Mode::NFM, 6250},
    {"FRS/GMRS", 462550000, 467725000, Mode::NFM, 12500},
    {"NOAA WX", 162400000, 162550000, Mode::NFM, 25000},
};
const int ScannerPage::kPresetCount =
    static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

const float ScannerPage::kHangTimes[] = {0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 10.0f};
const int ScannerPage::kHangCount =
    static_cast<int>(sizeof(kHangTimes) / sizeof(kHangTimes[0]));

ScannerPage::ScannerPage(Framebuffer& fb, SDRController& sdr, AudioOutput& audio,
                         InputHandler& input)
    : fb_(fb), sdr_(sdr), audio_(audio), input_(input), view_(fb),
      fft_(kFftSize), epoch_(std::chrono::steady_clock::now()) {}

ScannerPage::~ScannerPage() { cleanup(); }

bool ScannerPage::init() {
    loadSettings();
    return true;
}

void ScannerPage::cleanup() { stopWorker(); }

// ---------------------------------------------------------------------------
// Settings

void ScannerPage::loadSettings() {
    const auto& data = AppConfig::instance().data();
    if (!data.contains("scanner") || !data["scanner"].is_object()) {
        table_.setTolerance(raster_hz_);
        return;
    }
    const auto& s = data["scanner"];

    const uint32_t start = s.value("start_hz", start_hz_);
    const uint32_t stop = s.value("stop_hz", stop_hz_);
    if (start >= kMinHz && stop <= kMaxHz && stop > start && stop - start <= kMaxSpanHz) {
        start_hz_ = start;
        stop_hz_ = stop;
    }
    raster_hz_ = std::clamp<uint32_t>(s.value("raster_hz", raster_hz_), 1000, 100000);
    preset_ = s.value("preset", preset_);
    mode_ = std::clamp(s.value("mode", 0), 0, scanner::kModeCount - 1);
    threshold_db_ = std::clamp(s.value("threshold_db", 10.0f), 3.0f, 40.0f);
    hang_index_ = std::clamp(s.value("hang_index", 2), 0, kHangCount - 1);
    // gain_tenths is mapped onto the tuner's gain list in onEnter().
    saved_gain_tenths_ = s.value("gain_tenths", -1);

    std::vector<uint32_t> lockouts;
    if (s.contains("lockouts") && s["lockouts"].is_array()) {
        for (const auto& v : s["lockouts"]) {
            if (v.is_number_unsigned()) lockouts.push_back(v.get<uint32_t>());
        }
    }
    table_.setLockouts(lockouts);
    table_.setTolerance(raster_hz_);

    user_presets_.clear();
    if (s.contains("user_presets") && s["user_presets"].is_array()) {
        for (const auto& v : s["user_presets"]) {
            if (!v.is_object()) continue;
            Band b;
            b.name = v.value("name", std::string());
            b.start_hz = v.value("start_hz", 0u);
            b.stop_hz = v.value("stop_hz", 0u);
            b.raster_hz = std::clamp<uint32_t>(v.value("raster_hz", 5000u), 1000, 100000);
            b.mode = static_cast<Mode>(
                std::clamp(v.value("mode", 0), 0, scanner::kModeCount - 1));
            if (b.name.empty() || !validRange(b.start_hz, b.stop_hz)) continue;
            if (static_cast<int>(user_presets_.size()) >= kMaxUserPresets) break;
            user_presets_.push_back(b);
        }
    }
    all_mode_ = s.value("all_mode", false);
}

void ScannerPage::saveSettings() {
    auto& data = AppConfig::instance().data();
    if (!data.contains("scanner") || !data["scanner"].is_object()) {
        data["scanner"] = nlohmann::json::object();
    }
    auto& s = data["scanner"];
    s["start_hz"] = start_hz_;
    s["stop_hz"] = stop_hz_;
    s["raster_hz"] = raster_hz_;
    s["preset"] = preset_;
    s["mode"] = mode_.load();
    s["threshold_db"] = threshold_db_.load();
    s["hang_index"] = hang_index_.load();
    const int gi = gain_index_.load();
    s["gain_tenths"] = (gi > 0 && gi <= static_cast<int>(gain_options_.size()))
                           ? gain_options_[gi - 1]
                           : -1;
    s["all_mode"] = all_mode_;
    nlohmann::json users = nlohmann::json::array();
    for (const auto& b : user_presets_) {
        users.push_back({{"name", b.name},
                         {"start_hz", b.start_hz},
                         {"stop_hz", b.stop_hz},
                         {"mode", static_cast<int>(b.mode)},
                         {"raster_hz", b.raster_hz}});
    }
    s["user_presets"] = users;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s["lockouts"] = table_.lockouts();
    }
    AppConfig::instance().save();
    settings_dirty_ = false;
}

void ScannerPage::markDirty() {
    settings_dirty_ = true;
    dirty_since_ = std::chrono::steady_clock::now();
}

std::vector<ScannerPage::Band> ScannerPage::presetList() const {
    std::vector<Band> list;
    for (int i = 0; i < kPresetCount; ++i) {
        const Preset& p = kPresets[i];
        Band b;
        b.name = p.name;
        b.start_hz = p.start_hz;
        b.stop_hz = p.stop_hz;
        b.raster_hz = p.raster_hz;
        b.mode = p.mode;
        list.push_back(b);
    }
    list.insert(list.end(), user_presets_.begin(), user_presets_.end());
    return list;
}

std::string ScannerPage::nextUserName() const {
    for (int n = 1;; ++n) {
        const std::string name = "USER " + std::to_string(n);
        bool used = false;
        for (const auto& b : user_presets_) used = used || b.name == name;
        if (!used) return name;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle

void ScannerPage::onEnter() {
    std::cout << "ScannerPage: entering" << std::endl;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        state_ = ScannerState::Scan;
        tuned_hz_ = 0;
        hold_ = false;
    }

    // readsb / acarsdec may still own the dongle after the ADSB or ACARS
    // page.
    const int killed = std::system("pkill -9 readsb 2>/dev/null; pkill -9 acarsdec 2>/dev/null");
    (void)killed;

    if (!sdr_.isCapturing()) {
        usleep(800000);
        bool ok = false;
        for (int retry = 0; retry < 3 && !ok; ++retry) {
            if (retry > 0) usleep(500000);
            ok = sdr_.init(0);
        }
        if (!ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "SDR DEVICE BUSY";
            state_ = ScannerState::Error;
            return;
        }
        sdr_.setSampleRate(kSampleRate);
        sdr_.setFrequency(start_hz_);
        if (!sdr_.startCapture()) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "SDR CAPTURE FAILED";
            state_ = ScannerState::Error;
            return;
        }
    } else {
        sdr_.setSampleRate(kSampleRate);
    }

    // Offer roughly 10 dB manual gain steps from the tuner's own list.
    gain_options_.clear();
    const std::vector<int> supported = sdr_.getSupportedGainTenths();
    for (int want : {100, 200, 300, 400, 500}) {
        if (supported.empty()) break;
        int best = supported.front();
        for (int g : supported) {
            if (std::abs(g - want) < std::abs(best - want)) best = g;
        }
        if (std::find(gain_options_.begin(), gain_options_.end(), best) ==
            gain_options_.end()) {
            gain_options_.push_back(best);
        }
    }
    if (gain_options_.empty()) gain_options_ = {197, 297, 402, 496};
    int gi = 0;  // auto
    if (saved_gain_tenths_ >= 0) {
        for (size_t i = 0; i < gain_options_.size(); ++i) {
            if (gi == 0 || std::abs(gain_options_[i] - saved_gain_tenths_) <
                               std::abs(gain_options_[gi - 1] - saved_gain_tenths_)) {
                gi = static_cast<int>(i) + 1;
            }
        }
    }
    gain_index_ = gi;
    applied_gain_index_ = -2;

    audio_.setQuiet(true);
    startWorker();
}

void ScannerPage::onExit() {
    std::cout << "ScannerPage: exiting" << std::endl;
    stopWorker();
    if (settings_dirty_) saveSettings();
    audio_.setQuiet(false);
    sdr_.stopCapture();
    editing_ = false;
    help_visible_ = false;
}

void ScannerPage::startWorker() {
    stopWorker();
    rebuildBands();
    if (!sdr_.isCapturing()) return;
    run_ = true;
    worker_ = std::thread(&ScannerPage::workerLoop, this);
}

void ScannerPage::stopWorker() {
    run_ = false;
    if (worker_.joinable()) worker_.join();
}

double ScannerPage::now() const {
    // Offset keeps whole-second activity buckets positive.
    return 1000.0 + std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - epoch_).count();
}

// ---------------------------------------------------------------------------
// Worker (owns the dongle)

// Called with the worker stopped: the worker reads bands_ without a lock.
void ScannerPage::rebuildBands() {
    worker_all_ = all_mode_;
    bands_.clear();
    if (all_mode_) bands_ = presetList();
    if (bands_.empty()) {
        Band b;
        b.name = preset_;
        b.start_hz = start_hz_;
        b.stop_hz = stop_hz_;
        b.raster_hz = raster_hz_;
        b.mode = static_cast<Mode>(mode_.load());
        bands_.push_back(b);
    }
    band_phase_.assign(bands_.size(), 0);
    if (band_index_ >= bands_.size()) band_index_ = 0;

    // Signals from every band share one table: merge on the finest step.
    uint32_t tolerance = bands_.front().raster_hz;
    for (const auto& b : bands_) tolerance = std::min(tolerance, b.raster_hz);
    std::lock_guard<std::mutex> lock(mutex_);
    table_.setTolerance(tolerance);
    sweep_band_ = static_cast<int>(band_index_);
    tuned_band_ = 0;
}

// Band that holds `hz`; the band being swept wins where presets overlap.
int ScannerPage::bandFor(uint32_t hz) const {
    const auto contains = [&](int i) {
        return hz >= bands_[i].start_hz && hz <= bands_[i].stop_hz;
    };
    const int count = static_cast<int>(bands_.size());
    const int prefer = sweep_band_ < count ? sweep_band_ : 0;
    if (contains(prefer)) return prefer;
    for (int i = 0; i < count; ++i) {
        if (contains(i)) return i;
    }
    return prefer;
}

// ALL uses each preset's own mode; a single range follows the D key.
Mode ScannerPage::bandMode(int band) const {
    if (worker_all_ && band >= 0 && band < static_cast<int>(bands_.size())) {
        return bands_[band].mode;
    }
    return static_cast<Mode>(mode_.load());
}

scanner::SliceGeometry ScannerPage::geometry(double lo_hz, const Band& band) const {
    scanner::SliceGeometry geo;
    const size_t usable = static_cast<size_t>(kFftSize * kUsableFraction);
    geo.centre_bin = kFftSize / 2;
    geo.first_bin = geo.centre_bin - usable / 2;
    geo.last_bin = geo.first_bin + usable;
    geo.centre_hz = lo_hz;
    geo.bin_hz = static_cast<double>(kSampleRate) / kFftSize;
    geo.dc_guard_bins = 3;
    geo.min_hz = band.start_hz;
    geo.max_hz = band.stop_hz;
    return geo;
}

bool ScannerPage::readSamples(std::vector<uint8_t>& iq, size_t bytes) {
    for (int i = 0; i < 500; ++i) {
        if (!run_ || !sdr_.isCapturing()) return false;
        if (sdr_.getLatestSamples(iq, bytes)) return true;
        usleep(2000);
    }
    return false;
}

void ScannerPage::measureFft(const std::vector<uint8_t>& iq, int frames,
                             std::vector<float>& db_out) {
    const size_t seg = kFftSize * 2;
    if (iq.size() < seg || frames < 1) return;
    const size_t span = iq.size() - seg;
    scanner::SpectrumAverager avg;
    std::vector<float> mag;
    for (int f = 0; f < frames; ++f) {
        size_t off = frames > 1 ? span * f / (frames - 1) : 0;
        off &= ~static_cast<size_t>(1);
        fft_.process(iq.data() + off, seg, mag);
        avg.add(mag);
    }
    avg.result(db_out);
}

void ScannerPage::applyGain() {
    const int gi = gain_index_.load();
    if (gi == applied_gain_index_) return;
    if (gi <= 0 || gi > static_cast<int>(gain_options_.size())) {
        sdr_.setAGCMode(true);
    } else {
        // Manual mode first: setAGCMode(false) resets the gain stages.
        sdr_.setAGCMode(false);
        sdr_.setGainTenths(gain_options_[gi - 1]);
    }
    applied_gain_index_ = gi;
}

void ScannerPage::tuneTo(uint32_t hz) {
    // Normally the LO sits below the signal; near the bottom of the tuner's
    // range put it above instead.
    const double lo = hz >= kMinHz + kListenOffsetHz
                          ? static_cast<double>(hz) - kListenOffsetHz
                          : static_cast<double>(hz) + kListenOffsetHz;
    const int band = bandFor(hz);
    sdr_.setFrequency(static_cast<uint32_t>(lo));
    chan_.setMode(bandMode(band));
    chan_.setOffset(static_cast<double>(hz) - lo);
    chan_.reset();
    usleep(kSettleUs);
    sdr_.flushBuffer();

    std::cout << "ScannerPage: tuned " << hz / 1e6 << " MHz"
              << (hold_ ? " (hold)" : "") << std::endl;
    std::lock_guard<std::mutex> lock(mutex_);
    tuned_hz_ = hz;
    tuned_band_ = band;
    lo_hz_ = lo;
    last_active_ = now();
    active_ = false;
    state_ = hold_ ? ScannerState::Hold : ScannerState::Lock;
    scope_.clear();
}

// Leave the current signal: jump to another one heard moments ago inside the
// listening window, or go back to sweeping.
void ScannerPage::moveOn() {
    const double t = now();
    const double half = kSampleRate * kUsableFraction / 2.0;
    uint32_t next = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = table_.pickNext(t - 1.5, t, tuned_hz_,
                               static_cast<uint32_t>(std::max(0.0, lo_hz_ - half)),
                               static_cast<uint32_t>(lo_hz_ + half));
        hold_ = false;
        if (!next) {
            tuned_hz_ = 0;
            active_ = false;
            state_ = ScannerState::Scan;
        }
    }
    if (!next) std::cout << "ScannerPage: back to sweeping" << std::endl;
    if (next) tuneTo(next);
}

void ScannerPage::handleRequests() {
    const uint32_t tune = req_tune_hz_.exchange(0);
    if (tune) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hold_ = true;
        }
        tuneTo(tune);
    }

    if (req_clear_lockouts_.exchange(false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        table_.clearLockouts();
        lockouts_changed_ = true;
    }

    // The rest only apply while a frequency is tuned.
    if (tuned_hz_ == 0) {
        req_hold_toggle_ = false;
        req_skip_ = false;
        req_lockout_ = false;
        return;
    }

    if (req_hold_toggle_.exchange(false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_ = !hold_;
        if (hold_) {
            state_ = ScannerState::Hold;
        } else {
            // Resume: treat the carrier as just heard so a dead channel
            // still gets the normal hang time.
            state_ = ScannerState::Lock;
            last_active_ = now();
        }
    }

    if (req_lockout_.exchange(false)) {
        bool locked = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            locked = table_.toggleLockout(tuned_hz_);
            lockouts_changed_ = true;
        }
        if (locked) {
            moveOn();
            return;
        }
    }

    if (req_skip_.exchange(false)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            table_.snooze(tuned_hz_, now() + kSkipSnooze);
        }
        moveOn();
    }
}

void ScannerPage::workerLoop() {
    std::cout << "ScannerPage: worker started" << std::endl;
    applied_gain_index_ = -2;
    int failures = 0;
    while (run_) {
        applyGain();
        const Mode want = bandMode(tuned_band_);
        if (chan_.mode() != want) chan_.setMode(want);
        handleRequests();

        bool ok;
        if (tuned_hz_ == 0) {
            ok = sweepOnce();
        } else {
            ok = listenChunk();
        }
        if (!run_) break;

        failures = ok ? 0 : failures + 1;
        if (failures >= 5) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = "NO SAMPLES FROM SDR";
            state_ = ScannerState::Error;
            break;
        }
    }
    std::cout << "ScannerPage: worker stopped" << std::endl;
}

bool ScannerPage::sweepOnce() {
    // One band per call: in ALL mode the next call sweeps the next preset.
    const size_t bi = band_index_ < bands_.size() ? band_index_ : 0;
    const Band& band = bands_[bi];
    band_index_ = (bi + 1) % bands_.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sweep_band_ = static_cast<int>(bi);
        sweep_progress_ = 0;
    }

    const double sweep_start = now();
    const double range = std::max(1.0, static_cast<double>(band.stop_hz) - band.start_hz);
    const double max_slice = kSampleRate * kUsableFraction;
    const size_t steps = std::max<size_t>(1, static_cast<size_t>(std::ceil(range / max_slice)));
    const double slice = range / static_cast<double>(steps);
    // Alternate sweeps are shifted by half a slice, so a signal hidden under
    // one sweep's DC guard is caught by the next.
    const bool shifted = (band_phase_[bi]++ & 1) != 0;
    const size_t count = shifted ? steps + 1 : steps;
    const float threshold = threshold_db_.load();

    std::vector<uint8_t> iq;
    std::vector<float> db;
    bool any_data = false;

    for (size_t s = 0; s < count && run_; ++s) {
        // Abandon the sweep as soon as the user asks for something.
        if (req_tune_hz_.load() != 0 || req_clear_lockouts_.load()) return true;

        const double lo = shifted ? band.start_hz + s * slice
                                  : band.start_hz + (s + 0.5) * slice;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sweep_hz_ = static_cast<uint32_t>(lo);
        }
        sdr_.setFrequency(static_cast<uint32_t>(lo));
        usleep(kSettleUs);
        sdr_.flushBuffer();

        if (!readSamples(iq, kFftSize * 2 * kSweepFrames)) continue;
        any_data = true;
        measureFft(iq, kSweepFrames, db);
        const auto geo = geometry(lo, band);
        const auto result = scanner::detectSignals(db, geo, threshold, band.raster_hz);

        std::lock_guard<std::mutex> lock(mutex_);
        floor_db_ = floor_db_ == 0.0f ? result.floor_db
                                      : 0.8f * floor_db_ + 0.2f * result.floor_db;
        const double t = now();
        for (const auto& hit : result.hits) {
            if (!scanner::isCrystalHarmonic(hit.hz)) table_.observe(hit, t);
        }
        sweep_progress_ = static_cast<int>((s + 1) * 100 / count);
    }
    if (!run_) return true;

    // Park on the strongest signal heard during this sweep.
    const double t = now();
    uint32_t next = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        table_.expire(t, kKeepSeconds);
        next = table_.pickNext(sweep_start, t, 0, band.start_hz, band.stop_hz);
    }
    if (next) tuneTo(next);
    return any_data;
}

bool ScannerPage::listenChunk() {
    std::vector<uint8_t>& iq = listen_iq_;
    if (!readSamples(iq, kSampleRate / 20 * 2)) return false;

    std::vector<float> db;
    measureFft(iq, kListenFrames, db);
    const Band& band = bands_[static_cast<size_t>(tuned_band_)];
    const auto geo = geometry(lo_hz_, band);
    const float threshold = threshold_db_.load();
    const auto result = scanner::detectSignals(db, geo, threshold, band.raster_hz);
    const double offset = static_cast<double>(tuned_hz_) - lo_hz_;
    const double half_bw = chan_.mode() == Mode::AM ? 4000.0 : 6000.0;
    const float level = scanner::channelPeakDb(db, geo, offset, half_bw);
    const float snr = level - result.floor_db;

    std::vector<int16_t>& pcm = listen_pcm_;
    chan_.process(iq.data(), iq.size(), pcm);

    const double t = now();
    bool play = false;
    bool move_on = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool active = snr >= (active_ ? threshold - kHysteresisDb : threshold);
        active_ = active;
        tuned_snr_ = snr;
        floor_db_ = floor_db_ == 0.0f ? result.floor_db
                                      : 0.8f * floor_db_ + 0.2f * result.floor_db;
        af_level_ = std::clamp(chan_.audioRms() / 0.5f, 0.0f, 1.0f);

        // Other signals inside the listening window stay fresh. Hits within
        // the tuned channel's own bandwidth are its sidebands, not neighbours.
        const uint32_t own = std::max<uint32_t>(band.raster_hz, static_cast<uint32_t>(2 * half_bw));
        for (const auto& hit : result.hits) {
            const uint32_t d = hit.hz > tuned_hz_ ? hit.hz - tuned_hz_ : tuned_hz_ - hit.hz;
            if (d > own && !scanner::isCrystalHarmonic(hit.hz)) table_.observe(hit, t);
        }

        if (active) {
            last_active_ = t;
            table_.observe({tuned_hz_, level, snr}, t);
            if (state_ == ScannerState::Hang) state_ = ScannerState::Lock;
        } else if (!hold_) {
            const double quiet = t - last_active_;
            const double hang = kHangTimes[hang_index_.load()];
            if (state_ == ScannerState::Lock && quiet > kDropout) state_ = ScannerState::Hang;
            if (state_ == ScannerState::Hang && quiet >= kDropout + hang) move_on = true;
        }
        play = active || (t - last_active_) < kDropout;

        // Scope: the newest ~10 ms of audio.
        const size_t want = 480;
        const size_t from = pcm.size() > want ? pcm.size() - want : 0;
        scope_.resize(pcm.size() - from);
        for (size_t i = from; i < pcm.size(); ++i) scope_[i - from] = pcm[i] / 30000.0f;

        table_.expire(t, kKeepSeconds);
    }

    if (audio_.isOpen()) {
        // Keep feeding the sound card while parked so the stream never
        // underruns; silence when squelched or muted.
        if (!play || muted_.load()) std::fill(pcm.begin(), pcm.end(), 0);
        if (!pcm.empty()) audio_.write(pcm);
    }

    if (move_on) moveOn();
    return true;
}

// ---------------------------------------------------------------------------
// UI thread

void ScannerPage::showToast(const std::string& text) {
    toast_ = text;
    toast_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
}

void ScannerPage::onVolumeChanged(int percent) {
    char buf[16];
    if (percent >= 0) {
        std::snprintf(buf, sizeof(buf), "VOL %d%%", percent);
    } else {
        std::snprintf(buf, sizeof(buf), "NO VOL");
    }
    showToast(buf);
}

int ScannerPage::readBattery() {
    DIR* dir = opendir("/sys/class/power_supply");
    if (!dir) return -1;
    int percent = -1;
    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::string base = std::string("/sys/class/power_supply/") + entry->d_name;
        std::ifstream type(base + "/type");
        std::string t;
        if (!(type >> t) || t != "Battery") continue;
        std::ifstream cap(base + "/capacity");
        int value = -1;
        if (cap >> value && value >= 0 && value <= 100) {
            percent = value;
            break;
        }
    }
    closedir(dir);
    return percent;
}

void ScannerPage::fillSnapshot(ScannerSnapshot& snap) {
    const double t = now();
    const int64_t second = static_cast<int64_t>(std::floor(t));

    std::lock_guard<std::mutex> lock(mutex_);
    snap.state = state_;
    snap.error = error_;
    snap.all_mode = worker_all_;
    snap.band_count = static_cast<int>(bands_.size());
    const int shown = tuned_hz_ ? tuned_band_ : sweep_band_;
    const bool have_band = shown >= 0 && shown < static_cast<int>(bands_.size());
    snap.band = have_band ? bands_[shown].name : std::string();
    snap.mode = bandMode(shown);
    if (worker_all_ && !bands_.empty()) {
        // The signal list spans every preset in the rotation.
        snap.start_hz = bands_.front().start_hz;
        snap.stop_hz = bands_.front().stop_hz;
        for (const auto& b : bands_) {
            snap.start_hz = std::min(snap.start_hz, b.start_hz);
            snap.stop_hz = std::max(snap.stop_hz, b.stop_hz);
        }
        snap.preset = "ALL";
    } else {
        snap.start_hz = start_hz_;
        snap.stop_hz = stop_hz_;
        snap.preset = preset_;
    }
    snap.tuned_hz = tuned_hz_;
    snap.sweep_hz = sweep_hz_;
    snap.sweep_progress = sweep_progress_;
    snap.active = active_;
    snap.snr_db = tuned_snr_;
    snap.af_level = af_level_;
    snap.floor_db = floor_db_;
    snap.threshold_db = threshold_db_.load();
    const int gi = gain_index_.load();
    snap.gain_tenths = (gi > 0 && gi <= static_cast<int>(gain_options_.size()))
                           ? gain_options_[gi - 1]
                           : -1;
    snap.hang_s = kHangTimes[hang_index_.load()];
    snap.muted = muted_.load();
    snap.lockout_count = static_cast<int>(table_.lockouts().size());
    snap.scope = scope_;
    snap.tuned_id = 0;
    snap.duration_s = 0.0;
    snap.hang_left_s = 0.0;
    if (state_ == ScannerState::Hang) {
        snap.hang_left_s = last_active_ + kDropout + snap.hang_s - t;
    }

    snap.rows.clear();
    for (const auto& s : table_.signals()) {
        ScannerRow row;
        row.id = s.id;
        row.hz = s.hz;
        row.snr_db = s.snr_db;
        row.age_s = std::max(0.0, t - s.last_seen);
        row.duration_s = s.burstDuration();
        row.locked_out = table_.isLockedOut(s.hz);
        for (int b = 0; b < 32; ++b) {
            if (s.heardInSecond(second - b)) row.heard |= (uint64_t{1} << b);
        }
        if (tuned_hz_ != 0) {
            const uint32_t d = s.hz > tuned_hz_ ? s.hz - tuned_hz_ : tuned_hz_ - s.hz;
            if (d <= table_.tolerance()) {
                row.tuned = true;
                snap.tuned_id = s.id;
                snap.duration_s = s.burstDuration();
            }
        }
        snap.rows.push_back(row);
    }
    std::sort(snap.rows.begin(), snap.rows.end(),
              [](const ScannerRow& a, const ScannerRow& b) { return a.hz > b.hz; });

    // Only one row may claim the tuned frequency (the closest).
    bool seen = false;
    for (auto& row : snap.rows) {
        if (!row.tuned) continue;
        if (seen) row.tuned = false;
        seen = true;
    }

    const std::time_t wall = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&wall, &tm_now);
    char clock[16];
    std::strftime(clock, sizeof(clock), "%H:%M:%S", &tm_now);
    snap.clock = clock;
    snap.battery_percent = battery_;
    snap.toast = std::chrono::steady_clock::now() < toast_until_ ? toast_ : std::string();
}

void ScannerPage::update(float delta_time) {
    if (editing_) {
        view_.drawEditor(editor_);
        fb_.update();
        return;
    }

    // Arrow keys repeat while held (the Cardputer keyboard has no
    // kernel auto-repeat).
    if (held_key_ != APPKEY_NONE) {
        const int hold_ms = input_.getHoldDuration(held_key_);
        if (hold_ms <= 0 || help_visible_) {
            held_key_ = APPKEY_NONE;
            repeat_accum_ = 0.0f;
        } else if (hold_ms >= 400) {
            repeat_accum_ += delta_time;
            const float interval = hold_ms >= 1500 ? 0.07f : 0.15f;
            while (repeat_accum_ >= interval) {
                repeat_accum_ -= interval;
                handleKey(held_key_);
            }
        }
    }

    bool save_lockouts = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lockouts_changed_) {
            lockouts_changed_ = false;
            save_lockouts = true;
        }
    }
    if (save_lockouts) markDirty();
    if (settings_dirty_ &&
        std::chrono::steady_clock::now() - dirty_since_ > std::chrono::milliseconds(1500)) {
        saveSettings();
    }

    battery_accum_ += delta_time;
    if (battery_accum_ > 20.0f) {
        battery_accum_ = 0.0f;
        battery_ = readBattery();
    }

    render_accum_ += delta_time;
    if (render_accum_ < kRenderInterval) return;
    render_accum_ = 0.0f;

    fillSnapshot(snapshot_);
    view_.draw(snapshot_);
    if (help_visible_) view_.drawHelp();
    fb_.update();
}

void ScannerPage::stepSelection(int direction) {
    // Rows are listed high -> low, so "up" means a higher frequency.
    std::vector<uint32_t> freqs;
    uint32_t tuned = 0;
    uint32_t tolerance = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s : table_.signals()) freqs.push_back(s.hz);
        tuned = tuned_hz_;
        tolerance = table_.tolerance();
    }
    if (freqs.empty()) {
        showToast("NO SIGNALS YET");
        return;
    }
    std::sort(freqs.begin(), freqs.end(), std::greater<uint32_t>());
    int index = -1;
    for (size_t i = 0; i < freqs.size(); ++i) {
        const uint32_t d = freqs[i] > tuned ? freqs[i] - tuned : tuned - freqs[i];
        if (tuned && d <= tolerance) index = static_cast<int>(i);
    }
    if (index < 0) {
        index = 0;
    } else {
        index = std::clamp(index + direction, 0, static_cast<int>(freqs.size()) - 1);
    }
    req_tune_hz_ = freqs[index];
}

bool ScannerPage::handleInput(AppKeyCode key) {
    if (editing_) {
        handleEditorKey(key);
        return true;
    }
    if (help_visible_) {
        if (key == APPKEY_H || key == APPKEY_ESC) help_visible_ = false;
        return true;
    }
    if (key == APPKEY_UP || key == APPKEY_DOWN || key == APPKEY_LEFT ||
        key == APPKEY_RIGHT) {
        held_key_ = key;
        repeat_accum_ = 0.0f;
    }
    return handleKey(key);
}

bool ScannerPage::handleKey(AppKeyCode key) {
    ScannerState state;
    uint32_t tuned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
        tuned = tuned_hz_;
    }
    char buf[40];

    switch (key) {
        case APPKEY_H:
            help_visible_ = true;
            return true;
        case APPKEY_SPACE:
            if (!tuned) {
                showToast("NOTHING TUNED");
            } else {
                showToast(state == ScannerState::Hold ? "SCAN RESUMED" : "HOLDING");
                req_hold_toggle_ = true;
            }
            return true;
        case APPKEY_ENTER:
            if (!tuned) {
                showToast("SCANNING");
            } else {
                showToast("SKIPPED");
                req_skip_ = true;
            }
            return true;
        case APPKEY_UP:
            stepSelection(-1);
            return true;
        case APPKEY_DOWN:
            stepSelection(+1);
            return true;
        case APPKEY_LEFT:
        case APPKEY_RIGHT: {
            const float step = key == APPKEY_LEFT ? -1.0f : 1.0f;
            const float thr = std::clamp(threshold_db_.load() + step, 3.0f, 40.0f);
            threshold_db_ = thr;
            std::snprintf(buf, sizeof(buf), "THRESHOLD %.0f DB", thr);
            showToast(buf);
            markDirty();
            return true;
        }
        case APPKEY_L:
            if (!tuned) {
                showToast("TUNE A SIGNAL FIRST");
            } else {
                bool locked;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    locked = table_.isLockedOut(tuned);
                }
                showToast(locked ? "LOCKOUT REMOVED" : "LOCKED OUT");
                req_lockout_ = true;
            }
            return true;
        case APPKEY_BACKSPACE:
            req_clear_lockouts_ = true;
            showToast("LOCKOUTS CLEARED");
            return true;
        case APPKEY_D: {
            if (all_mode_) {
                showToast("ALL: MODE SET PER PRESET");
                return true;
            }
            const int m = (mode_.load() + 1) % scanner::kModeCount;
            mode_ = m;
            std::snprintf(buf, sizeof(buf), "MODE %s", scanner::modeName(static_cast<Mode>(m)));
            showToast(buf);
            markDirty();
            return true;
        }
        case APPKEY_G: {
            const int count = static_cast<int>(gain_options_.size()) + 1;
            const int gi = (gain_index_.load() + 1) % count;
            gain_index_ = gi;
            if (gi == 0) {
                showToast("GAIN AUTO");
            } else {
                std::snprintf(buf, sizeof(buf), "GAIN %.1f DB", gain_options_[gi - 1] / 10.0);
                showToast(buf);
            }
            markDirty();
            return true;
        }
        case APPKEY_T: {
            const int hi = (hang_index_.load() + 1) % kHangCount;
            hang_index_ = hi;
            std::snprintf(buf, sizeof(buf), "HANG %.1f S", kHangTimes[hi]);
            showToast(buf);
            markDirty();
            return true;
        }
        case APPKEY_M:
            muted_ = !muted_.load();
            showToast(muted_ ? "MUTED" : "SOUND ON");
            return true;
        case APPKEY_R:
            openEditor();
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Range editor
//
// The PRESET row cycles ALL, the built-in presets, the user's presets and
// "+ ADD NEW". Editing a field of a preset turns it into a custom range;
// under + ADD NEW the edits are saved as a new preset on ENTER.

int ScannerPage::editorChoiceCount() const {
    return kPresetCount + static_cast<int>(user_presets_.size()) + 2;
}

void ScannerPage::openEditor() {
    stopWorker();
    editing_ = true;
    editor_fresh_ = true;
    editor_delete_armed_ = false;
    editor_.field = 0;
    editor_.error.clear();
    editor_.start = formatMhz(start_hz_);
    editor_.stop = formatMhz(stop_hz_);
    editor_.mode = static_cast<Mode>(mode_.load());
    editor_raster_ = raster_hz_;
    editor_choice_ = -1;
    if (all_mode_) {
        editor_choice_ = 0;
    } else if (!preset_.empty()) {
        const auto list = presetList();
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].name == preset_) editor_choice_ = static_cast<int>(i) + 1;
        }
    }
    syncEditorView();
}

void ScannerPage::selectEditorChoice(int choice) {
    const auto list = presetList();
    editor_choice_ = choice;
    editor_delete_armed_ = false;
    editor_fresh_ = true;
    editor_.error.clear();
    if (choice >= 1 && choice <= static_cast<int>(list.size())) {
        const Band& b = list[static_cast<size_t>(choice - 1)];
        editor_.start = formatMhz(b.start_hz);
        editor_.stop = formatMhz(b.stop_hz);
        editor_.mode = b.mode;
        editor_raster_ = b.raster_hz;
    }
    // ALL and + ADD NEW keep the fields: a new preset starts from the range
    // that was on screen.
    syncEditorView();
}

void ScannerPage::syncEditorView() {
    const auto list = presetList();
    const int count = static_cast<int>(list.size());
    editor_.all = editor_choice_ == 0;
    editor_.adding = editor_choice_ == count + 1;
    editor_.user_preset = editor_choice_ > kPresetCount && editor_choice_ <= count;
    if (editor_.all) {
        editor_.preset = "ALL";
        editor_.field = 0;
    } else if (editor_.adding) {
        editor_.preset = "+ ADD NEW";
    } else if (editor_choice_ >= 1) {
        editor_.preset = list[static_cast<size_t>(editor_choice_ - 1)].name;
    } else {
        editor_.preset = "CUSTOM";
    }
    editor_.step = formatStepKhz(editor_raster_);
    editor_.all_names.clear();
    for (const auto& b : list) editor_.all_names.push_back(b.name);

    editor_.info.clear();
    editor_.info_alert = false;
    if (editor_delete_armed_) {
        editor_.info = "BKSP AGAIN TO DELETE " + editor_.preset;
        editor_.info_alert = true;
    } else if (editor_.adding) {
        if (static_cast<int>(user_presets_.size()) >= kMaxUserPresets) {
            editor_.info = "PRESET LIST FULL - DELETE ONE FIRST";
            editor_.info_alert = true;
        } else {
            editor_.info = "ENTER SAVES AS " + nextUserName();
        }
    } else if (editor_.user_preset && editor_.field == 0) {
        editor_.info = "BKSP DELETES THIS PRESET";
    }
}

void ScannerPage::closeEditor() {
    editing_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = error_.empty() ? ScannerState::Scan : ScannerState::Error;
        tuned_hz_ = 0;
        hold_ = false;
    }
    startWorker();
}

void ScannerPage::applyEditor() {
    if (editor_.all) {
        if (!all_mode_) {
            all_mode_ = true;
            std::lock_guard<std::mutex> lock(mutex_);
            table_.clear();
            sweep_progress_ = 0;
        }
        saveSettings();
        closeEditor();
        return;
    }

    double start_mhz = 0.0;
    double stop_mhz = 0.0;
    try {
        start_mhz = std::stod(editor_.start);
        stop_mhz = std::stod(editor_.stop);
    } catch (...) {
        editor_.error = "INVALID NUMBER";
        return;
    }
    const uint32_t start = static_cast<uint32_t>(std::llround(start_mhz * 1e6));
    const uint32_t stop = static_cast<uint32_t>(std::llround(stop_mhz * 1e6));
    if (start < kMinHz || stop > kMaxHz || stop <= start + kMinSpanHz) {
        editor_.error = "RANGE MUST BE 24-1766 MHZ";
        return;
    }
    if (stop - start > kMaxSpanHz) {
        editor_.error = "SPAN TOO WIDE (MAX 300 MHZ)";
        return;
    }

    const auto list = presetList();
    if (editor_.adding) {
        if (static_cast<int>(user_presets_.size()) >= kMaxUserPresets) {
            editor_.error = "PRESET LIST FULL";
            return;
        }
        for (const auto& b : list) {
            if (b.start_hz == start && b.stop_hz == stop && b.mode == editor_.mode &&
                b.raster_hz == editor_raster_) {
                editor_.error = "SAME AS " + b.name;
                return;
            }
        }
        Band b;
        b.name = nextUserName();
        b.start_hz = start;
        b.stop_hz = stop;
        b.raster_hz = editor_raster_;
        b.mode = editor_.mode;
        user_presets_.push_back(b);
        preset_ = b.name;
        showToast("SAVED " + b.name);
    } else if (editor_choice_ >= 1 && editor_choice_ <= static_cast<int>(list.size())) {
        preset_ = list[static_cast<size_t>(editor_choice_ - 1)].name;
    } else {
        preset_.clear();
    }

    const bool changed = all_mode_ || start != start_hz_ || stop != stop_hz_ ||
                         editor_raster_ != raster_hz_;
    all_mode_ = false;
    start_hz_ = start;
    stop_hz_ = stop;
    raster_hz_ = editor_raster_;
    mode_ = static_cast<int>(editor_.mode);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (changed) table_.clear();
        sweep_progress_ = 0;
    }
    saveSettings();
    closeEditor();
}

void ScannerPage::handleEditorKey(AppKeyCode key) {
    // Editing a preset's values makes it a custom range; under + ADD NEW the
    // edits belong to the preset being added.
    const auto edited = [&]() {
        if (!editor_.adding) editor_choice_ = -1;
        editor_.error.clear();
    };
    const auto toggleMode = [&]() {
        editor_.mode = static_cast<Mode>((static_cast<int>(editor_.mode) + 1) %
                                         scanner::kModeCount);
        edited();
    };

    if (key == APPKEY_ESC) {
        closeEditor();
        return;
    }

    if (key == APPKEY_LEFT || key == APPKEY_RIGHT) {
        const int dir = key == APPKEY_LEFT ? -1 : 1;
        if (editor_.field == 0) {
            const int count = editorChoiceCount();
            int c = editor_choice_;
            if (c < 0) c = dir > 0 ? -1 : 0;
            selectEditorChoice((c + dir + count) % count);
            return;
        }
        if (editor_.field == 3) toggleMode();
        if (editor_.field == 4) {
            int index = 0;
            while (index < kStepCount - 1 && kSteps[index] < editor_raster_) ++index;
            if (kSteps[index] == editor_raster_ || dir < 0) {
                index = (index + dir + kStepCount) % kStepCount;
            }
            editor_raster_ = kSteps[index];
            edited();
        }
        syncEditorView();
        return;
    }

    if (key == APPKEY_UP || key == APPKEY_DOWN) {
        if (!editor_.all) {
            editor_.field = (editor_.field + (key == APPKEY_DOWN ? 1 : 4)) % 5;
        }
        editor_fresh_ = true;
        editor_delete_armed_ = false;
        editor_.error.clear();
        syncEditorView();
        return;
    }

    if (key == APPKEY_D) {
        if (!editor_.all) toggleMode();
        syncEditorView();
        return;
    }

    if (key == APPKEY_ENTER) {
        if (editor_.field == 1) {
            editor_.field = 2;
            editor_fresh_ = true;
            syncEditorView();
        } else {
            applyEditor();
        }
        return;
    }

    if (key == APPKEY_BACKSPACE && editor_.field == 0) {
        if (!editor_.user_preset) return;
        if (!editor_delete_armed_) {
            editor_delete_armed_ = true;
            syncEditorView();
            return;
        }
        const size_t index = static_cast<size_t>(editor_choice_ - 1 - kPresetCount);
        const std::string name = user_presets_[index].name;
        user_presets_.erase(user_presets_.begin() + static_cast<long>(index));
        if (preset_ == name) preset_.clear();
        saveSettings();
        selectEditorChoice(editor_choice_ - 1);
        editor_.info = "DELETED " + name;
        return;
    }

    std::string* buffer = nullptr;
    if (editor_.field == 1) buffer = &editor_.start;
    if (editor_.field == 2) buffer = &editor_.stop;
    if (!buffer) return;

    if (key >= APPKEY_0 && key <= APPKEY_9) {
        if (editor_fresh_) buffer->clear();
        editor_fresh_ = false;
        if (buffer->size() < 9) *buffer += static_cast<char>('0' + (key - APPKEY_0));
        edited();
    } else if (key == APPKEY_DOT) {
        if (editor_fresh_) buffer->clear();
        editor_fresh_ = false;
        if (buffer->find('.') == std::string::npos && buffer->size() < 9) *buffer += '.';
        edited();
    } else if (key == APPKEY_BACKSPACE) {
        if (editor_fresh_) {
            buffer->clear();
        } else if (!buffer->empty()) {
            buffer->pop_back();
        }
        editor_fresh_ = false;
        edited();
    } else {
        return;
    }
    syncEditorView();
}
