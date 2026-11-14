#include "pages/spectrum_page.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr float kRenderInterval = 1.0f / 15.0f;
constexpr int kSettleUs = 15000;

std::string rangePath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/home/pi") +
           "/.config/zerosdr/spectrum_range.txt";
}

void ensureParentDirs(const std::string& path) {
    std::string dir;
    for (size_t i = 0; i < path.size(); ++i) {
        dir += path[i];
        if (path[i] == '/' && i + 1 < path.size()) {
            mkdir(dir.c_str(), 0755);
        }
    }
}

uint16_t lerpRgb565(uint16_t a, uint16_t b, float t) {
    const int ar = (a >> 11) & 0x1F;
    const int ag = (a >> 5) & 0x3F;
    const int ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F;
    const int bg = (b >> 5) & 0x3F;
    const int bb = b & 0x1F;
    const int r = ar + static_cast<int>((br - ar) * t + 0.5f);
    const int g = ag + static_cast<int>((bg - ag) * t + 0.5f);
    const int bl = ab + static_cast<int>((bb - ab) * t + 0.5f);
    return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

}  // namespace

SpectrumPage::SpectrumPage(Framebuffer& fb, SDRController& sdr,
                           InputHandler& input)
    : fb_(fb), sdr_(sdr), input_(input), ui_(fb), fft_(kFftSize) {
    wf_pixels_.assign(kWaterfallH * kWidth, COLOR_BG_DARK);
    row_db_.assign(kWidth, -120.0f);
    disp_db_.assign(kWidth, -120.0f);
}

SpectrumPage::~SpectrumPage() { cleanup(); }

bool SpectrumPage::init() {
    loadRange();
    rebuildGeometry();
    return true;
}

void SpectrumPage::cleanup() { stopSweep(); }

void SpectrumPage::loadRange() {
    std::ifstream in(rangePath());
    uint32_t start = 0;
    uint32_t stop = 0;
    if (in >> start >> stop && start >= 24000000 && stop <= 1766000000 &&
        stop > start + 100000) {
        start_hz_ = start;
        stop_hz_ = stop;
        range_configured_ = true;
    }
}

void SpectrumPage::saveRange() const {
    const std::string path = rangePath();
    ensureParentDirs(path);
    std::ofstream out(path, std::ios::trunc);
    if (out.is_open()) {
        out << start_hz_ << " " << stop_hz_ << "\n";
    }
}

void SpectrumPage::rebuildGeometry() {
    const double range =
        stop_hz_ > start_hz_ ? static_cast<double>(stop_hz_ - start_hz_)
                             : 1000000.0;
    const double max_slice = kSampleRate * kUsableFraction;
    step_count_ = static_cast<size_t>(std::ceil(range / max_slice));
    if (step_count_ < 1) step_count_ = 1;
    slice_hz_ = range / static_cast<double>(step_count_);
    fft_bin_hz_ = static_cast<double>(kSampleRate) /
                  static_cast<double>(kFftSize);
    slice_bins_ = static_cast<size_t>(std::floor(slice_hz_ / fft_bin_hz_));
    if (slice_bins_ < 8) slice_bins_ = 8;
    if (slice_bins_ > kFftSize) slice_bins_ = kFftSize;

    total_bins_ = step_count_ * slice_bins_;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        bins_db_.assign(total_bins_, -120.0f);
    }
    view_start_ = 0;
    view_bins_ = total_bins_;
    floor_db_ = -100.0f;
    ceil_db_ = -20.0f;
    clearWaterfall();
}

void SpectrumPage::onEnter() {
    status_error_.clear();
    std::cout << "SpectrumPage: Entering wideband mode" << std::endl;

    // readsb may still own the dongle after the ADSB page.
    system("pkill -9 readsb 2>/dev/null");

    if (!sdr_.isCapturing()) {
        usleep(800000);
        bool ok = false;
        for (int retry = 0; retry < 3 && !ok; ++retry) {
            if (retry > 0) usleep(500000);
            if (sdr_.init(0)) ok = true;
        }
        if (!ok) {
            status_error_ = "SDR DEVICE BUSY";
            fb_.clear(COLOR_BG_DARK);
            fb_.update();
            return;
        }
        sdr_.setSampleRate(kSampleRate);
        sdr_.setFrequency(start_hz_);
        sdr_.setAutoGain(true);
        sdr_.setAGCMode(true);
        if (!sdr_.startCapture()) {
            status_error_ = "SDR CAPTURE FAILED";
            return;
        }
    } else {
        sdr_.setSampleRate(kSampleRate);
    }

    rebuildGeometry();
    if (!range_configured_) {
        openEditor();
    } else {
        startSweep();
    }
}

void SpectrumPage::onExit() {
    std::cout << "SpectrumPage: Exiting wideband mode" << std::endl;
    stopSweep();
    sdr_.stopCapture();
}

void SpectrumPage::startSweep() {
    stopSweep();
    if (!sdr_.isCapturing()) {
        status_error_ = "SDR NOT CAPTURING";
        return;
    }
    status_error_.clear();
    sweep_run_ = true;
    sweep_thread_ = std::thread(&SpectrumPage::sweepLoop, this);
}

void SpectrumPage::stopSweep() {
    sweep_run_ = false;
    if (sweep_thread_.joinable()) sweep_thread_.join();
}

bool SpectrumPage::waitSamples(std::vector<uint8_t>& iq, size_t bytes) {
    for (int i = 0; i < 250; ++i) {
        if (!sweep_run_) return false;
        if (!sdr_.isCapturing()) return false;
        if (sdr_.getLatestSamples(iq, bytes)) return true;
        usleep(1000);
    }
    return false;
}

void SpectrumPage::sweepLoop() {
    std::vector<uint8_t> iq;
    std::vector<float> mag;
    std::vector<float> acc;

    while (sweep_run_) {
        sweep_progress_ = 0;
        const size_t steps = step_count_;
        const size_t sbins = slice_bins_;
        const double shz = slice_hz_;
        const int first_bin =
            static_cast<int>(kFftSize / 2) - static_cast<int>(sbins / 2);

        for (size_t s = 0; s < steps && sweep_run_; ++s) {
            const uint32_t center = start_hz_ + static_cast<uint32_t>(
                                                   std::llround((s + 0.5) * shz));
            sweep_freq_ = center;
            sdr_.setFrequency(center);
            usleep(kSettleUs);
            sdr_.flushBuffer();

            acc.assign(sbins, 0.0f);
            int frames = 0;
            for (int f = 0; f < 5 && sweep_run_; ++f) {
                if (!waitSamples(iq, kFftSize * 2)) break;
                fft_.process(iq, mag);
                for (size_t j = 0; j < sbins; ++j) {
                    const int idx = first_bin + static_cast<int>(j);
                    if (idx >= 0 && idx < static_cast<int>(mag.size())) {
                        acc[j] += mag[idx];
                    }
                }
                ++frames;
            }

            if (frames > 0) {
                std::lock_guard<std::mutex> lock(data_mutex_);
                const size_t off = s * sbins;
                if (off + sbins <= bins_db_.size()) {
                    for (size_t j = 0; j < sbins; ++j) {
                        bins_db_[off + j] = acc[j] / static_cast<float>(frames);
                    }
                }
            }

            sweep_progress_ =
                static_cast<int>((s + 1) * 100 / std::max<size_t>(1, steps));
        }

        // Short pause before starting the next sweep.
        for (int i = 0; i < 8 && sweep_run_; ++i) usleep(10000);
    }
}

void SpectrumPage::update(float delta_time) {
    if (editing_) {
        drawEditor();
        return;
    }
    if (!status_error_.empty()) {
        fb_.clear(COLOR_BG_DARK);
        ui_.drawText(10, 80, status_error_.c_str(), COLOR_RED);
        fb_.update();
        return;
    }
    render_accum_ += delta_time;
    if (render_accum_ < kRenderInterval) return;
    render_accum_ = 0.0f;
    drawDisplay();
}

bool SpectrumPage::handleInput(AppKeyCode key) {
    if (editing_) {
        handleEditorKey(key);
        return true;
    }

    const size_t min_bins = std::min<size_t>(32, total_bins_);
    switch (key) {
        case APPKEY_LEFT: {
            const size_t step = std::max<size_t>(1, view_bins_ / 8);
            view_start_ = view_start_ > step ? view_start_ - step : 0;
            clearWaterfall();
            return true;
        }
        case APPKEY_RIGHT: {
            const size_t step = std::max<size_t>(1, view_bins_ / 8);
            const size_t max_start =
                total_bins_ > view_bins_ ? total_bins_ - view_bins_ : 0;
            view_start_ = std::min(max_start, view_start_ + step);
            clearWaterfall();
            return true;
        }
        case APPKEY_UP: {
            const size_t center = view_start_ + view_bins_ / 2;
            size_t bins = std::max(min_bins, view_bins_ / 2);
            if (bins > total_bins_) bins = total_bins_;
            view_bins_ = bins;
            view_start_ = center > bins / 2 ? center - bins / 2 : 0;
            if (view_start_ + view_bins_ > total_bins_) {
                view_start_ = total_bins_ > view_bins_ ? total_bins_ - view_bins_
                                                       : 0;
            }
            clearWaterfall();
            return true;
        }
        case APPKEY_DOWN: {
            const size_t center = view_start_ + view_bins_ / 2;
            size_t bins = std::min(total_bins_, view_bins_ * 2);
            if (bins < min_bins) bins = min_bins;
            view_bins_ = bins;
            view_start_ = center > bins / 2 ? center - bins / 2 : 0;
            if (view_start_ + view_bins_ > total_bins_) {
                view_start_ = total_bins_ > view_bins_ ? total_bins_ - view_bins_
                                                       : 0;
            }
            clearWaterfall();
            return true;
        }
        case APPKEY_R:
            openEditor();
            return true;
        case APPKEY_ENTER:
            view_start_ = 0;
            view_bins_ = total_bins_;
            clearWaterfall();
            return true;
        default:
            return false;
    }
}

void SpectrumPage::openEditor() {
    stopSweep();
    editing_ = true;
    edit_stop_field_ = false;
    edit_fresh_ = true;
    edit_error_.clear();

    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.3f", start_hz_ / 1e6);
    edit_start_ = buf;
    std::snprintf(buf, sizeof(buf), "%.3f", stop_hz_ / 1e6);
    edit_stop_ = buf;
}

void SpectrumPage::applyEditor() {
    float start_mhz = 0.0f;
    float stop_mhz = 0.0f;
    try {
        start_mhz = std::stof(edit_start_);
        stop_mhz = std::stof(edit_stop_);
    } catch (...) {
        edit_error_ = "INVALID NUMBER";
        return;
    }

    const uint32_t start = static_cast<uint32_t>(
        std::llround(static_cast<double>(start_mhz) * 1e6));
    const uint32_t stop = static_cast<uint32_t>(
        std::llround(static_cast<double>(stop_mhz) * 1e6));

    if (start < 24000000 || stop > 1766000000 || stop < start + 500000) {
        edit_error_ = "INVALID RANGE (24-1766 MHz)";
        return;
    }

    start_hz_ = start;
    stop_hz_ = stop;
    range_configured_ = true;
    saveRange();
    editing_ = false;
    rebuildGeometry();
    startSweep();
}

void SpectrumPage::handleEditorKey(AppKeyCode key) {
    std::string* buffer = edit_stop_field_ ? &edit_stop_ : &edit_start_;

    if (key >= APPKEY_0 && key <= APPKEY_9) {
        if (edit_fresh_) {
            buffer->clear();
            edit_fresh_ = false;
        }
        if (buffer->size() < 9) {
            *buffer += static_cast<char>('0' + (key - APPKEY_0));
        }
        edit_error_.clear();
        return;
    }
    if (key == APPKEY_DOT) {
        if (edit_fresh_) {
            buffer->clear();
            edit_fresh_ = false;
        }
        if (buffer->find('.') == std::string::npos && buffer->size() < 9) {
            *buffer += '.';
        }
        edit_error_.clear();
        return;
    }
    if (key == APPKEY_BACKSPACE) {
        if (edit_fresh_) {
            buffer->clear();
            edit_fresh_ = false;
        } else if (!buffer->empty()) {
            buffer->pop_back();
        }
        edit_error_.clear();
        return;
    }
    if (key == APPKEY_UP || key == APPKEY_DOWN) {
        edit_stop_field_ = !edit_stop_field_;
        edit_fresh_ = true;
        edit_error_.clear();
        return;
    }
    if (key == APPKEY_ENTER) {
        if (!edit_stop_field_) {
            edit_stop_field_ = true;
            edit_fresh_ = true;
        } else {
            applyEditor();
        }
        return;
    }
    if (key == APPKEY_ESC) {
        editing_ = false;
        if (!range_configured_) {
            range_configured_ = true;
            start_hz_ = 88000000;
            stop_hz_ = 108000000;
            saveRange();
        }
        rebuildGeometry();
        startSweep();
    }
}

void SpectrumPage::clearWaterfall() {
    std::fill(wf_pixels_.begin(), wf_pixels_.end(), COLOR_BG_DARK);
    std::fill(disp_db_.begin(), disp_db_.end(), -120.0f);
}

std::string SpectrumPage::formatMhz(uint32_t hz, int decimals) const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.*fM", decimals, hz / 1e6);
    return std::string(buf);
}

uint16_t SpectrumPage::heatColor(float t) const {
    struct Stop {
        float pos;
        uint16_t color;
    };
    static const Stop stops[] = {
        {0.00f, 0x0000}, {0.18f, 0x0010}, {0.34f, 0x001F},
        {0.50f, 0x03FF}, {0.64f, 0x07E0}, {0.80f, 0xFFE0},
        {0.90f, 0xFC00}, {1.00f, 0xF800},
    };
    t = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 1; i < sizeof(stops) / sizeof(stops[0]); ++i) {
        if (t <= stops[i].pos) {
            const float span = stops[i].pos - stops[i - 1].pos;
            const float k = span > 0.0f ? (t - stops[i - 1].pos) / span : 0.0f;
            return lerpRgb565(stops[i - 1].color, stops[i].color, k);
        }
    }
    return stops[sizeof(stops) / sizeof(stops[0]) - 1].color;
}

void SpectrumPage::drawDisplay() {
    fb_.clear(COLOR_BG_DARK);
    drawSpectrum();
    drawAxis();
    drawWaterfall();
    drawStatusBar();
    fb_.update();
}

void SpectrumPage::drawSpectrum() {
    float min_db = 1e9f;
    float max_db = -1e9f;

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        const size_t bins = std::max<size_t>(1, view_bins_);
        for (int x = 0; x < kWidth; ++x) {
            const size_t start = view_start_ + (static_cast<size_t>(x) * bins) /
                                                   static_cast<size_t>(kWidth);
            size_t end = view_start_ + (static_cast<size_t>(x + 1) * bins) /
                                           static_cast<size_t>(kWidth);
            if (end <= start) end = start + 1;
            if (end > bins_db_.size()) end = bins_db_.size();
            float value = -120.0f;
            for (size_t i = start; i < end; ++i) {
                if (bins_db_[i] > value) value = bins_db_[i];
            }
            row_db_[x] = value;
            if (value > -119.0f) {
                min_db = std::min(min_db, value);
                max_db = std::max(max_db, value);
            }
        }
    }

    // Spatial [1 2 1] smoothing plus a light temporal blend removes the
    // pixel-to-pixel FFT noise that makes the trace look jagged.
    std::vector<float> smoothed(kWidth);
    for (int x = 0; x < kWidth; ++x) {
        const float left = row_db_[x > 0 ? x - 1 : 0];
        const float right = row_db_[x + 1 < kWidth ? x + 1 : kWidth - 1];
        smoothed[x] = (left + 2.0f * row_db_[x] + right) * 0.25f;
    }
    for (int x = 0; x < kWidth; ++x) {
        if (disp_db_[x] <= -119.0f) {
            disp_db_[x] = smoothed[x];
        } else {
            disp_db_[x] = disp_db_[x] * 0.55f + smoothed[x] * 0.45f;
        }
        row_db_[x] = disp_db_[x];
        if (row_db_[x] > -119.0f) {
            min_db = std::min(min_db, row_db_[x]);
            max_db = std::max(max_db, row_db_[x]);
        }
    }

    if (min_db > max_db) {
        min_db = -100.0f;
        max_db = -20.0f;
    }
    if (max_db - min_db < 10.0f) {
        const float mid = (max_db + min_db) * 0.5f;
        min_db = mid - 5.0f;
        max_db = mid + 5.0f;
    }
    floor_db_ = 0.9f * floor_db_ + 0.1f * min_db;
    ceil_db_ = 0.9f * ceil_db_ + 0.1f * max_db;
    if (ceil_db_ - floor_db_ < 5.0f) ceil_db_ = floor_db_ + 5.0f;

    // Grid.
    for (int i = 1; i <= 3; ++i) {
        const int y = (i * kSpectrumH) / 4;
        for (int x = 0; x < kWidth; x += 4) {
            fb_.drawPixel(x, y, COLOR_GRID);
            fb_.drawPixel(x + 1, y, COLOR_GRID);
        }
    }

    const float span = ceil_db_ - floor_db_;
    int prev_x = 0;
    int prev_y = kSpectrumH - 1;
    for (int x = 0; x < kWidth; ++x) {
        float norm = (row_db_[x] - floor_db_) / span;
        norm = std::clamp(norm, 0.0f, 1.0f);
        const int y = kSpectrumH - 1 -
                      static_cast<int>(norm * static_cast<float>(kSpectrumH - 3));
        if (x == 0) {
            prev_x = x;
            prev_y = y;
        } else {
            fb_.drawLine(prev_x, prev_y, x, y, 0xFFE0);
            prev_x = x;
            prev_y = y;
        }
    }
}

void SpectrumPage::drawAxis() {
    fb_.fillRect(0, kAxisY, kWidth, kAxisH, 0x0841);
    fb_.fillRect(0, kAxisY, kWidth, 1, COLOR_GRID);

    const double range = static_cast<double>(stop_hz_ - start_hz_);
    const double bin_hz = range / std::max<size_t>(1, total_bins_);
    const double view_span =
        bin_hz * static_cast<double>(std::max<size_t>(1, view_bins_));
    int decimals = 1;
    if (view_span < 1000000.0) decimals = 3;
    else if (view_span < 10000000.0) decimals = 2;

    static const int fractions[5] = {0, 1, 2, 3, 4};
    for (int i = 0; i < 5; ++i) {
        const int x = (fractions[i] * (kWidth - 1)) / 4;
        fb_.drawPixel(x, kAxisY, COLOR_GRID);
        // Only label the left, center and right ticks so the wide 6x8
        // labels cannot overlap each other.
        if (i != 0 && i != 2 && i != 4) continue;
        const size_t bin = view_start_ +
                           (static_cast<size_t>(x) * view_bins_) / kWidth;
        const uint32_t hz = start_hz_ + static_cast<uint32_t>(bin * bin_hz);
        const std::string label = formatMhz(hz, decimals);
        int label_x = x;
        const int label_w = static_cast<int>(label.size()) * 7;
        if (label_x + label_w > kWidth - 1) label_x = kWidth - 1 - label_w;
        if (label_x < 0) label_x = 0;
        ui_.drawText(label_x, kAxisY + 3, label.c_str(), COLOR_TEXT_DIM);
    }
}

void SpectrumPage::drawWaterfall() {
    // Scroll the history down first, then write the new top row.
    if (kWaterfallH > 1) {
        std::memmove(&wf_pixels_[kWidth], &wf_pixels_[0],
                     (kWaterfallH - 1) * kWidth * sizeof(uint16_t));
    }

    const float span = ceil_db_ - floor_db_;
    for (int x = 0; x < kWidth; ++x) {
        const float norm = std::clamp((row_db_[x] - floor_db_) / span, 0.0f,
                                      1.0f);
        // A gamma keeps the noise floor dark while peaks stay bright.
        wf_pixels_[x] = heatColor(std::pow(norm, 1.6f));
    }

    for (int row = 0; row < kWaterfallH; ++row) {
        for (int x = 0; x < kWidth; ++x) {
            fb_.drawPixel(x, kWaterfallY + row, wf_pixels_[row * kWidth + x]);
        }
    }
}

void SpectrumPage::drawStatusBar() {
    fb_.fillRect(0, kStatusY, kWidth, 170 - kStatusY, COLOR_STATUSBAR);

    const double range = static_cast<double>(stop_hz_ - start_hz_);
    const double bin_hz = range / std::max<size_t>(1, total_bins_);
    const double view_span =
        bin_hz * static_cast<double>(std::max<size_t>(1, view_bins_));

    int decimals = 1;
    if (view_span < 1000000.0) decimals = 3;
    else if (view_span < 10000000.0) decimals = 2;

    const uint32_t visible_start = start_hz_ + static_cast<uint32_t>(
                                                   view_start_ * bin_hz);
    const uint32_t visible_end =
        start_hz_ + static_cast<uint32_t>((view_start_ + view_bins_) * bin_hz);
    const std::string left = formatMhz(visible_start, decimals) + "-" +
                             formatMhz(visible_end, decimals);
    ui_.drawText(2, kStatusY + 3, left.c_str(), COLOR_TEXT_BRIGHT);

    char span_buf[24];
    std::snprintf(span_buf, sizeof(span_buf), "SPAN %.2fM", view_span / 1e6);
    ui_.drawText(178, kStatusY + 3, span_buf, COLOR_TEXT_DIM);

    char progress[16];
    std::snprintf(progress, sizeof(progress), "%3d%%", sweep_progress_.load());
    ui_.drawText(kWidth - 2 - 4 * 7, kStatusY + 3, progress, COLOR_YELLOW);
}

void SpectrumPage::drawEditor() {
    fb_.clear(COLOR_BG_DARK);
    fb_.drawRect(1, 1, kWidth - 2, 168, COLOR_TEXT_DIM);

    const char* title = "CUSTOM RANGE (MHz)";
    const int title_w = static_cast<int>(std::strlen(title)) * 7;
    ui_.drawText((kWidth - title_w) / 2, 14, title, COLOR_YELLOW);

    auto draw_row = [&](int y, const char* label, const std::string& value,
                        bool active, bool show_mhz) {
        if (active) {
            fb_.fillRect(12, y - 4, kWidth - 24, 16, COLOR_SELECTED);
            ui_.drawText(16, y, ">", COLOR_YELLOW);
        }
        ui_.drawText(28, y, label, active ? COLOR_WHITE : COLOR_TEXT_BRIGHT);
        ui_.drawText(100, y, value.c_str(),
                     active ? COLOR_WHITE : COLOR_TEXT_BRIGHT);
        if (show_mhz) {
            ui_.drawText(100 + static_cast<int>(value.size()) * 7 + 6, y, "MHz",
                         COLOR_TEXT_DIM);
        }
    };

    draw_row(52, "START", edit_start_, !edit_stop_field_, true);
    draw_row(84, "STOP", edit_stop_, edit_stop_field_, true);

    ui_.drawText(12, 112, "DIGITS .  BACKSPACE", COLOR_TEXT_DIM);
    ui_.drawText(12, 124, "UP/DOWN FIELD   ENTER NEXT / START", COLOR_TEXT_DIM);

    if (!edit_error_.empty()) {
        ui_.drawText(12, 142, edit_error_.c_str(), COLOR_RED);
    }

    fb_.update();
}
