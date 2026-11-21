#include "pages/rfi_page.h"
#include "screenshot.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace {
struct Band { const char* name; uint32_t low, high; };
// Receive survey spans, not transmit privileges. Regional allocations vary.
constexpr Band bands[] = {{"10m",28000000,29700000}, {"20m",14000000,14350000},
                          {"40m",7000000,7300000}, {"80m",3500000,4000000}};
constexpr size_t fft_size = 1024;
constexpr uint32_t sample_rate = 1024000;
constexpr int wf_y = 99, wf_h = 42, width = 320;
}

RfiPage::RfiPage(Framebuffer& fb, SDRController& sdr) : fb_(fb), sdr_(sdr), ui_(fb) {
    fft_.setIQImbalanceCorrection(false);
    waterfall_.assign(width * wf_h, COLOR_BG_DARK);
}

void RfiPage::clearReference() {
    reference_.reset();
    live_.clear();
    std::fill(waterfall_.begin(), waterfall_.end(), COLOR_BG_DARK);
    last_sweep_ = {};
}

void RfiPage::onEnter() {
    saved_frequency_ = sdr_.getFrequency();
    saved_rate_ = sdr_.getSampleRate();
    saved_gain_ = sdr_.getGainTenths();
    saved_agc_ = sdr_.getAGCEnabled();
    help_ = false;
    start();
}

void RfiPage::onExit() {
    stop();
    sdr_.stopCapture();
    // Restore shared receiver configuration before returning to another page.
    sdr_.setSampleRate(saved_rate_);
    sdr_.setFrequency(saved_frequency_);
    sdr_.setAGCMode(saved_agc_);
    if (!saved_agc_) sdr_.setGainTenths(saved_gain_);
    sdr_.close();
}

void RfiPage::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void RfiPage::start() {
    stop();
    sdr_.stopCapture();
    clearReference();
    error_.clear();
    // ADSB hands off the USB receiver through the same mechanism as Spectrum.
    std::system("pkill -9 readsb 2>/dev/null");
    usleep(500000);
    if (!sdr_.init(0) || !sdr_.setSampleRate(sample_rate) ||
        !sdr_.setAGCMode(false) ||
        !sdr_.setFrequency((bands[band_].low + bands[band_].high) / 2)) {
        error_ = "SDR SETUP FAILED - R RETRY";
        sdr_.close();
        return;
    }
    const auto gains = sdr_.getSupportedGainTenths();
    if (gains.empty()) {
        error_ = "NO MANUAL GAINS - R RETRY";
        sdr_.close();
        return;
    }
    const int selected = *std::min_element(gains.begin(), gains.end(), [&](int a, int b) {
        return std::abs(a - gain_ * 10) < std::abs(b - gain_ * 10);
    });
    if (!sdr_.setGainTenths(selected)) {
        error_ = "GAIN FAILED - R RETRY";
        sdr_.close();
        return;
    }
    actual_gain_ = sdr_.getGainTenths() / 10.0f;
    if (!sdr_.startCapture()) {
        error_ = "SDR CAPTURE FAILED - R RETRY";
        sdr_.close();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        captured_.clear();
        scan_error_.clear();
        sequence_ = displayed_ = 0;
    }
    running_ = true;
    worker_ = std::thread(&RfiPage::scan, this);
}

bool RfiPage::samples(std::vector<uint8_t>& iq, size_t bytes) {
    for (int attempt = 0; attempt < 1000 && running_; ++attempt) {
        if (sdr_.getLatestSamples(iq, bytes)) return true;
        usleep(1000);
    }
    return false;
}

void RfiPage::scan() {
    const auto band = bands[band_];
    const unsigned steps = static_cast<unsigned>(std::ceil((band.high - band.low) / 640000.0));
    const double slice = (band.high - band.low) / static_cast<double>(steps);
    // Keep exact 1 kHz FFT spacing; centers partition the requested span.
    const size_t bins = static_cast<size_t>(std::floor(slice / 1000.0));
    std::vector<uint8_t> iq;
    std::vector<float> magnitude;
    while (running_) {
        std::vector<float> sweep(steps * bins, 0.0f);
        std::string failure;
        for (unsigned step = 0; step < steps && running_; ++step) {
            const uint32_t center = band.low + static_cast<uint32_t>(std::llround((step + 0.5) * slice));
            if (!sdr_.setFrequency(center)) { failure = "TUNE FAILED - R RETRY"; break; }
            // Drain both stale USB samples and the application ring after tuning.
            for (int i = 0; i < 8 && running_; ++i) usleep(10000);
            sdr_.flushBuffer();
            if (!samples(iq, 65536)) { failure = "NO IQ SAMPLES - R RETRY"; break; }
            for (unsigned frame = 0; frame < 16 && running_; ++frame) {
                if (!samples(iq, fft_size * 2)) { failure = "NO IQ SAMPLES - R RETRY"; break; }
                fft_.process(iq, magnitude);
                for (size_t j = 0; j < bins; ++j) {
                    const size_t bin = fft_size / 2 - bins / 2 + j;
                    sweep[step * bins + j] += RfiReference::fftPower(magnitude[bin], fft_size) / 16.0f;
                }
            }
            if (!failure.empty()) break;
        }
        if (!running_) break;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scan_error_ = failure;
            if (failure.empty()) {
                captured_ = std::move(sweep);
                if (++sequence_ == 1) std::cout << "RFI: first complete sweep, " << band.name
                    << ", " << captured_.size() << " bins, gain " << actual_gain_ << " dB" << std::endl;
            } else {
                std::cerr << "RFI: " << failure << std::endl;
            }
        }
        if (!failure.empty()) break;
        for (int i = 0; i < 10 && running_; ++i) usleep(10000);
    }
}

void RfiPage::update(float delta_time) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!scan_error_.empty()) { error_ = scan_error_; reference_.reset(); }
        if (sequence_ != displayed_ && error_.empty()) {
            live_ = captured_;
            displayed_ = sequence_;
            last_sweep_ = std::chrono::steady_clock::now();
            const bool was_collecting = reference_.collecting();
            reference_.add(live_);
            if (was_collecting && reference_.ready()) {
                std::cout << "RFI: baseline ready (8 complete sweeps), " << bands[band_].name << std::endl;
            }
            std::move_backward(waterfall_.begin(), waterfall_.end() - width, waterfall_.end());
            for (int x = 0; x < width; ++x) {
                const size_t first = static_cast<size_t>(x) * live_.size() / width;
                const size_t end = std::min(live_.size(), std::max(first + 1,
                    static_cast<size_t>(x + 1) * live_.size() / width));
                if (!reference_.ready()) { waterfall_[x] = COLOR_BG_DARK; continue; }
                float change = 0;
                for (size_t i = first; i < end; ++i) {
                    const float delta = RfiReference::db(live_[i]) - RfiReference::db(reference_.power()[i]);
                    if (std::fabs(delta) > std::fabs(change)) change = delta;
                }
                const float strength = std::clamp(std::fabs(change) / 20.0f, 0.0f, 1.0f);
                // Red = increase, cyan = decrease; dark means unchanged.
                waterfall_[x] = change >= 0 ? static_cast<uint16_t>(static_cast<int>(31 * strength) << 11)
                    : static_cast<uint16_t>((static_cast<int>(63 * strength) << 5) | static_cast<int>(31 * strength));
            }
        }
    }
    draw_elapsed_ += delta_time;
    if (draw_elapsed_ >= 0.1f) { draw_elapsed_ = 0; draw(); }
}

bool RfiPage::handleInput(AppKeyCode key) {
    if (key == APPKEY_H) { help_ = !help_; return true; }
    if (help_) { if (key == APPKEY_ESC) help_ = false; return true; }
    switch (key) {
        case APPKEY_LEFT: stop(); band_ = (band_ + 3) % 4; start(); return true;
        case APPKEY_RIGHT: stop(); band_ = (band_ + 1) % 4; start(); return true;
        case APPKEY_UP: stop(); gain_ = std::min(49, gain_ + 5); start(); return true;
        case APPKEY_DOWN: stop(); gain_ = std::max(0, gain_ - 5); start(); return true;
        case APPKEY_ENTER:
        case APPKEY_BW:
            if (error_.empty() && !live_.empty()) {
                // Exclude any scan already in flight from the new reference.
                stop();
                { std::lock_guard<std::mutex> lock(mutex_); displayed_ = sequence_; }
                reference_.begin();
                std::fill(waterfall_.begin(), waterfall_.end(), COLOR_BG_DARK);
                running_ = true;
                worker_ = std::thread(&RfiPage::scan, this);
            }
            return true;
        case APPKEY_SPACE: delta_view_ = !delta_view_; return true;
        case APPKEY_R: start(); return true;
        case APPKEY_SCREENSHOT: {
            const auto path = makeScreenshotPath();
            if (fb_.saveScreenshot(path.c_str())) std::cout << "RFI screenshot: " << path << std::endl;
            return true;
        }
        default: return false;
    }
}

void RfiPage::draw() {
    fb_.clear(COLOR_BG_DARK);
    char text[80];
    std::snprintf(text, sizeof(text), "RFI %s  GAIN %.1fdB  %s", bands[band_].name, actual_gain_, delta_view_ ? "DELTA" : "LIVE");
    ui_.drawText(2, 2, text, COLOR_WHITE);
    if (!error_.empty()) {
        ui_.drawText(4, 45, error_.c_str(), COLOR_RED);
        ui_.drawText(4, 65, "CHECK SDR / HF DRIVER SUPPORT", COLOR_TEXT_DIM);
    } else {
        for (int y : {20, 50, 80}) fb_.drawLine(0, y, width - 1, y, COLOR_GRID);
        auto yFor = [&](float value) {
            return delta_view_ ? 50 - static_cast<int>(std::clamp(value, -20.0f, 20.0f) * 1.5f)
                               : 80 - static_cast<int>((std::clamp(value, -100.0f, 0.0f) + 100.0f) * 0.6f);
        };
        int prev_live = 0, prev_ref = 0;
        for (int x = 0; x < width && !live_.empty(); ++x) {
            const size_t first = static_cast<size_t>(x) * live_.size() / width;
            const size_t end = std::min(live_.size(), std::max(first + 1,
                static_cast<size_t>(x + 1) * live_.size() / width));
            float live_db = -200, base = -200, change = 0;
            for (size_t i = first; i < end; ++i) {
                const float value = RfiReference::db(live_[i]);
                live_db = std::max(live_db, value);
                if (reference_.ready()) {
                    const float ref = RfiReference::db(reference_.power()[i]);
                    base = std::max(base, ref);
                    if (std::fabs(value - ref) > std::fabs(change)) change = value - ref;
                }
            }
            const int live_y = yFor(delta_view_ ? change : live_db);
            const int ref_y = yFor(delta_view_ ? 0 : base);
            if (x && (!delta_view_ || reference_.ready())) fb_.drawLine(x - 1, prev_live, x, live_y, COLOR_GREEN);
            if (x && reference_.ready()) fb_.drawLine(x - 1, prev_ref, x, ref_y, COLOR_YELLOW);
            prev_live = live_y; prev_ref = ref_y;
        }
        ui_.drawText(2, 15, delta_view_ ? "+20dB" : "0 REL", COLOR_TEXT_DIM);
        ui_.drawText(2, 73, delta_view_ ? "-20dB" : "-100", COLOR_TEXT_DIM);
    }
    std::snprintf(text, sizeof(text), "%.3f", bands[band_].low / 1e6);
    ui_.drawText(2, 87, text, COLOR_TEXT_DIM);
    ui_.drawText(100, 87, "MHz / 1kHz bins", COLOR_TEXT_DIM);
    std::snprintf(text, sizeof(text), "%.3f", bands[band_].high / 1e6);
    ui_.drawText(270, 87, text, COLOR_TEXT_DIM);
    for (int y = 0; y < wf_h; ++y) for (int x = 0; x < width; ++x)
        fb_.drawPixel(x, wf_y + y, waterfall_[y * width + x]);
    if (reference_.collecting()) std::snprintf(text, sizeof(text), "BASELINE %u/%u KEEP EQUIPMENT ON", reference_.count(), RfiReference::kSweeps);
    else if (reference_.ready()) {
        const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - last_sweep_).count();
        std::snprintf(text, sizeof(text), "MEDIAN %+.1fdB  AGE %.1fs", reference_.medianChange(live_), age);
    } else std::snprintf(text, sizeof(text), "ENTER: BASELINE WITH EQUIPMENT ON");
    ui_.drawText(2, 144, text, COLOR_YELLOW);
    ui_.drawText(2, 158, "<> BAND  UP/DN GAIN  H HELP  TAB MENU", COLOR_TEXT_BRIGHT);
    if (help_) {
        fb_.fillRect(8, 12, 304, 142, COLOR_DIALOG_BG);
        const char* lines[] = {"RFI: EQUIPMENT ON/OFF COMPARISON", "<> BAND: 10m / 20m / 40m / 80m",
            "UP/DOWN: FIXED GAIN (RESETS REF)", "ENTER/B: CAPTURE 8-SWEEP BASELINE", "THEN SWITCH SUSPECT EQUIPMENT OFF",
            "GREEN LIVE / YELLOW BASELINE", "SPACE: LIVE OR +/-20dB GRAPH", "WATERFALL: RED UP / CYAN DOWN",
            "MEDIAN: TYPICAL BIN POWER CHANGE", "R RETRY/RESET  FN+P SCREENSHOT", "RELATIVE LEVELS; NOT CALIBRATED dBm", "H CLOSE / TAB MENU"};
        for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i)
            ui_.drawText(12, 17 + static_cast<int>(i) * 11, lines[i], COLOR_WHITE);
    }
    fb_.update();
}
