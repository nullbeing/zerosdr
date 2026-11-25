#include "pages/spectrum_page.h"
#include "app_config.h"
#include "system_volume.h"

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

struct WfStop {
    float pos;
    uint16_t color;
};

struct WfTheme {
    const char* name;
    WfStop stops[8];
};

// Waterfall ramps. CLASSIC keeps the original blue/green/yellow/red look;
// the others match the SDR page's palettes.
constexpr WfTheme kWfThemes[] = {
    {"CLASSIC", {{0.00f, 0x0000}, {0.18f, 0x0010}, {0.34f, 0x001F},
                 {0.50f, 0x03FF}, {0.64f, 0x07E0}, {0.80f, 0xFFE0},
                 {0.90f, 0xFC00}, {1.00f, 0xF800}}},
    {"DEFAULT", {{0.00f, 0x0862}, {0.14f, 0x1045}, {0.28f, 0x188A},
                 {0.42f, 0x1958}, {0.56f, 0x22DF}, {0.70f, 0x455F},
                 {0.85f, 0x96DF}, {1.00f, 0xFFFF}}},
    {"TURBO", {{0.00f, 0x3087}, {0.14f, 0x4235}, {0.28f, 0x3D1F},
               {0.42f, 0x1E7A}, {0.56f, 0x67ED}, {0.70f, 0xD746},
               {0.85f, 0xFCC5}, {1.00f, 0xB0C0}}},
    {"INFERNO", {{0.00f, 0x0000}, {0.14f, 0x1868}, {0.28f, 0x486D},
                 {0.42f, 0x78ED}, {0.56f, 0xA16C}, {0.70f, 0xEB44},
                 {0.85f, 0xF687}, {1.00f, 0xFFF4}}},
    {"VIRIDIS", {{0.00f, 0x400A}, {0.14f, 0x3A51}, {0.28f, 0x3351},
                 {0.42f, 0x1CF1}, {0.56f, 0x35AF}, {0.70f, 0x6E6B},
                 {0.85f, 0xB6E5}, {1.00f, 0xFF24}}},
    {"PLASMA", {{0.00f, 0x0850}, {0.14f, 0x4013}, {0.28f, 0x7015},
                {0.42f, 0x98B3}, {0.56f, 0xB9B0}, {0.70f, 0xDAAD},
                {0.85f, 0xEBCA}, {1.00f, 0xF7C4}}},
};

constexpr int kWfThemeCount = sizeof(kWfThemes) / sizeof(kWfThemes[0]);

}  // namespace

SpectrumPage::SpectrumPage(Framebuffer& fb, SDRController& sdr,
                           InputHandler& input)
    : fb_(fb), sdr_(sdr), input_(input), ui_(fb), fft_(kFftSize) {
    wf_pixels_.assign(kWaterfallH * kWidth, COLOR_BG_DARK);
    row_db_.assign(kWidth, -120.0f);
    disp_db_.assign(kWidth, -120.0f);
    hold_row_db_.assign(kWidth, -120.0f);
}

SpectrumPage::~SpectrumPage() { cleanup(); }

bool SpectrumPage::init() {
    loadRange();
    rebuildGeometry();
    return true;
}

void SpectrumPage::cleanup() { stopSweep(); }

void SpectrumPage::loadRange() {
    const auto& data = AppConfig::instance().data();
    if (!data.contains("spectrum") || !data["spectrum"].is_object()) return;
    const auto& spectrum = data["spectrum"];

    max_hold_decay_db_s_ = spectrum.value("max_hold_decay_db_s", 1.5f);
    if (max_hold_decay_db_s_ < 0.0f) max_hold_decay_db_s_ = 0.0f;
    if (max_hold_decay_db_s_ > 20.0f) max_hold_decay_db_s_ = 20.0f;

    theme_index_ = spectrum.value("theme", 0);
    if (theme_index_ < 0 || theme_index_ >= kWfThemeCount) theme_index_ = 0;
    peak_labels_ = spectrum.value("peak_labels", true);

    if (!spectrum.value("configured", false)) return;

    const uint32_t start = spectrum.value("start_hz", 88000000u);
    const uint32_t stop = spectrum.value("stop_hz", 108000000u);
    if (start >= 24000000 && stop <= 1766000000 &&
        stop > start + 100000) {
        start_hz_ = start;
        stop_hz_ = stop;
        range_configured_ = true;
    }
}

void SpectrumPage::saveRange() const {
    auto& data = AppConfig::instance().data();
    if (!data.contains("spectrum") || !data["spectrum"].is_object()) {
        data["spectrum"] = nlohmann::json::object();
    }
    auto& spectrum = data["spectrum"];
    spectrum["configured"] = true;
    spectrum["start_hz"] = start_hz_;
    spectrum["stop_hz"] = stop_hz_;
    AppConfig::instance().save();
}

void SpectrumPage::saveTheme() const {
    auto& data = AppConfig::instance().data();
    if (!data.contains("spectrum") || !data["spectrum"].is_object()) {
        data["spectrum"] = nlohmann::json::object();
    }
    data["spectrum"]["theme"] = theme_index_;
    AppConfig::instance().save();
}

void SpectrumPage::savePeakLabels() const {
    auto& data = AppConfig::instance().data();
    if (!data.contains("spectrum") || !data["spectrum"].is_object()) {
        data["spectrum"] = nlohmann::json::object();
    }
    data["spectrum"]["peak_labels"] = peak_labels_;
    AppConfig::instance().save();
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
        max_hold_db_.assign(total_bins_, -120.0f);
    }
    std::fill(hold_row_db_.begin(), hold_row_db_.end(), -120.0f);
    slice_floor_db_.assign(step_count_, -1000.0f);
    reference_floor_db_ = -1000.0f;
    view_start_ = 0;
    view_bins_ = total_bins_;
    floor_db_ = -100.0f;
    ceil_db_ = -20.0f;
    scale_initialized_ = false;
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
        if (!sdr_.startCapture()) {
            status_error_ = "SDR CAPTURE FAILED";
            return;
        }
    } else {
        sdr_.setSampleRate(kSampleRate);
    }

    // The wideband sweep always uses the tuner's automatic gain. Fixed manual
    // gain amplifies the RTL-SDR's 28.8 MHz clock spur and makes the display
    // look like a comb.
    sdr_.setAGCMode(true);

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

    auto last_sweep_start = std::chrono::steady_clock::now();

    while (sweep_run_) {
        const auto sweep_start = std::chrono::steady_clock::now();
        const float sweep_period = std::chrono::duration<float>(
            sweep_start - last_sweep_start).count();
        last_sweep_start = sweep_start;
        sweep_progress_ = 0;
        const size_t steps = step_count_;
        const size_t sbins = slice_bins_;
        const double shz = slice_hz_;
        if (slice_floor_db_.size() != steps) {
            slice_floor_db_.assign(steps, -1000.0f);
        }
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
                std::vector<float> values(sbins);
                for (size_t j = 0; j < sbins; ++j) {
                    values[j] = acc[j] / static_cast<float>(frames);
                }

                // Per-slice noise floor (20th percentile). The tuner AGC can
                // pick a different gain for a slice with strong signals,
                // which would otherwise show as a step in the stitched trace.
                std::vector<float> sorted = values;
                const size_t floor_idx =
                    std::min(sbins - 1, std::max<size_t>(1, sbins / 5));
                std::nth_element(sorted.begin(), sorted.begin() + floor_idx,
                                 sorted.end());
                const float floor_now = sorted[floor_idx];
                if (s < slice_floor_db_.size()) {
                    float& stored = slice_floor_db_[s];
                    stored = stored < -900.0f
                                 ? floor_now
                                 : 0.85f * stored + 0.15f * floor_now;
                }
                float offset = 0.0f;
                if (reference_floor_db_ > -900.0f &&
                    s < slice_floor_db_.size()) {
                    offset = reference_floor_db_ - slice_floor_db_[s];
                    offset = std::clamp(offset, -20.0f, 20.0f);
                }

                std::lock_guard<std::mutex> lock(data_mutex_);
                const size_t off = s * sbins;
                if (off + sbins <= bins_db_.size()) {
                    const bool hold = max_hold_active_.load();
                    const bool have_hold = hold && off + sbins <= max_hold_db_.size();
                    const float decay = max_hold_decay_db_s_ * sweep_period;
                    for (size_t j = 0; j < sbins; ++j) {
                        const float value = values[j] + offset;
                        bins_db_[off + j] = value;
                        if (have_hold) {
                            // Age the envelope once per sweep, then let the
                            // new value win if it is higher. A persistent
                            // signal keeps its level; a peak that is gone
                            // falls by max_hold_decay_db_s_ dB per second.
                            float held = max_hold_db_[off + j] - decay;
                            if (held < -120.0f) held = -120.0f;
                            if (value > held) held = value;
                            max_hold_db_[off + j] = held;
                        }
                    }
                }
            }

            sweep_progress_ =
                static_cast<int>((s + 1) * 100 / std::max<size_t>(1, steps));
        }

        // Anchor the per-slice offsets to a slowly moving common floor.
        {
            std::vector<float> floors;
            floors.reserve(slice_floor_db_.size());
            for (float f : slice_floor_db_) {
                if (f > -900.0f) floors.push_back(f);
            }
            if (!floors.empty()) {
                const size_t mid = floors.size() / 2;
                std::nth_element(floors.begin(), floors.begin() + mid,
                                 floors.end());
                const float median = floors[mid];
                reference_floor_db_ =
                    reference_floor_db_ < -900.0f
                        ? median
                        : 0.95f * reference_floor_db_ + 0.05f * median;
            }
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
    if (help_visible_) {
        if (key == APPKEY_H || key == APPKEY_ESC) {
            help_visible_ = false;
        }
        return true;
    }
    if (key == APPKEY_H) {
        help_visible_ = true;
        return true;
    }

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
        case APPKEY_UP:
        case APPKEY_PLUS:
            zoomView(true);
            return true;
        case APPKEY_DOWN:
        case APPKEY_MINUS:
            zoomView(false);
            return true;
        case APPKEY_SPACE:
            smooth_display_ = !smooth_display_;
            toast_ = smooth_display_ ? "SMOOTH ON" : "SMOOTH OFF";
            toast_until_ = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(1500);
            return true;
        case APPKEY_P: {
            const bool next = !max_hold_active_.load();
            max_hold_active_ = next;
            std::lock_guard<std::mutex> lock(data_mutex_);
            std::fill(max_hold_db_.begin(), max_hold_db_.end(), -120.0f);
            std::fill(hold_row_db_.begin(), hold_row_db_.end(), -120.0f);
            toast_ = next ? "MAX HOLD ON" : "MAX HOLD OFF";
            toast_until_ = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(1500);
            return true;
        }
        case APPKEY_M: {
            theme_index_ = (theme_index_ + 1) % kWfThemeCount;
            toast_ = std::string("THEME: ") + kWfThemes[theme_index_].name;
            toast_until_ = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(1500);
            saveTheme();
            return true;
        }
        case APPKEY_L:
            peak_labels_ = !peak_labels_;
            toast_ = peak_labels_ ? "PEAK LABELS ON" : "PEAK LABELS OFF";
            toast_until_ = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(1500);
            savePeakLabels();
            return true;
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

void SpectrumPage::zoomView(bool zoom_in) {
    const size_t min_bins = std::min<size_t>(32, total_bins_);
    const size_t center = view_start_ + view_bins_ / 2;
    size_t bins = zoom_in ? std::max(min_bins, view_bins_ / 2)
                          : std::min(total_bins_, view_bins_ * 2);
    if (bins < min_bins) bins = min_bins;
    if (bins > total_bins_) bins = total_bins_;
    view_bins_ = bins;
    view_start_ = center > bins / 2 ? center - bins / 2 : 0;
    if (view_start_ + view_bins_ > total_bins_) {
        view_start_ = total_bins_ > view_bins_ ? total_bins_ - view_bins_ : 0;
    }
    clearWaterfall();
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
    const WfTheme& theme = kWfThemes[theme_index_];
    t = std::clamp(t, 0.0f, 1.0f);
    for (size_t i = 1; i < 8; ++i) {
        if (t <= theme.stops[i].pos) {
            const float span = theme.stops[i].pos - theme.stops[i - 1].pos;
            const float k =
                span > 0.0f ? (t - theme.stops[i - 1].pos) / span : 0.0f;
            return lerpRgb565(theme.stops[i - 1].color,
                              theme.stops[i].color, k);
        }
    }
    return theme.stops[7].color;
}

void SpectrumPage::drawDisplay() {
    fb_.clear(COLOR_BG_DARK);
    drawSpectrum();
    drawAxis();
    drawWaterfall();
    drawStatusBar();
    if (help_visible_) drawHelpOverlay();
    fb_.update();
}

void SpectrumPage::drawRoundedPanel(int x, int y, int w, int h, int radius,
                                    uint16_t color, float alpha) {
    if (w <= 0 || h <= 0) return;
    radius = std::max(0, std::min(radius, std::min(w / 2, h / 2)));
    for (int row = 0; row < h; ++row) {
        int inset = 0;
        if (row < radius) {
            const float dy = radius - (row + 0.5f);
            inset = radius - static_cast<int>(std::lround(
                                 std::sqrt(radius * radius - dy * dy)));
        } else if (row >= h - radius) {
            const float dy = (row + 0.5f) - (h - radius);
            inset = radius - static_cast<int>(std::lround(
                                 std::sqrt(radius * radius - dy * dy)));
        }
        for (int col = 0; col < w - 2 * inset; ++col) {
            fb_.blendPixel(x + inset + col, y + row, color, alpha);
        }
    }
}

void SpectrumPage::drawHelpOverlay() {
    // Dim the page, then lay a translucent rounded panel on top.
    for (int y = 0; y < 170; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            fb_.blendPixel(x, y, COLOR_BLACK, 0.45f);
        }
    }

    const int panel_w = 252;
    const int panel_h = 158;
    const int radius = 9;
    const int px = (kWidth - panel_w) / 2;
    const int py = (170 - panel_h) / 2;
    drawRoundedPanel(px, py, panel_w, panel_h, radius, COLOR_DIALOG_BG, 0.85f);

    const char* title = "SPECTRUM HELP";
    const int title_w = static_cast<int>(std::strlen(title)) * 7;
    ui_.drawText(px + (panel_w - title_w) / 2, py + 8, title, COLOR_YELLOW);

    static const char* const keys[] = {
        "LEFT/RIGHT", "UP/DN +/-", "ENTER",  "P",     "M",
        "L",          "SPACE",     "R",      "TAB",   "FN+D/S",
        "FN+P",       "HOLD ESC",  "H",
    };
    static const char* const actions[] = {
        "PAN",             "ZOOM",        "FULL SPAN",
        "MAX HOLD (DECAY)", "WATERFALL THEME", "PEAK LABELS",
        "SMOOTH",          "SET RANGE",   "MENU",
        "VOLUME",          "SCREENSHOT",  "EXIT",
        "CLOSE",
    };

    const int row_y = py + 22;
    const int row_step = 9;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const int y = row_y + static_cast<int>(i) * row_step;
        ui_.drawText(px + 16, y, keys[i], COLOR_YELLOW);
        ui_.drawText(px + 100, y, actions[i], COLOR_TEXT_BRIGHT);
    }
}

void SpectrumPage::smoothTrace(std::vector<float>& values, int window) const {
    if (window < 3 || values.size() < 3) return;
    if ((window & 1) == 0) ++window;
    const int half = window / 2;
    std::vector<float> out(values.size());
    std::vector<float> buf(static_cast<size_t>(window));
    for (size_t i = 0; i < values.size(); ++i) {
        for (int k = -half; k <= half; ++k) {
            const int idx = std::clamp<int>(
                static_cast<int>(i) + k, 0,
                static_cast<int>(values.size()) - 1);
            buf[static_cast<size_t>(k + half)] =
                values[static_cast<size_t>(idx)];
        }
        std::nth_element(buf.begin(), buf.begin() + half, buf.end());
        out[i] = buf[static_cast<size_t>(half)];
    }
    values.swap(out);
}

void SpectrumPage::drawSpectrum() {
    const bool hold_active = max_hold_active_.load();

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        const size_t bins = std::max<size_t>(1, view_bins_);
        const bool have_hold =
            hold_active && max_hold_db_.size() == bins_db_.size();
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

            float held = -120.0f;
            if (have_hold) {
                for (size_t i = start; i < end; ++i) {
                    if (max_hold_db_[i] > held) held = max_hold_db_[i];
                }
            }
            hold_row_db_[x] = held;
        }
    }

    if (smooth_display_) {
        // Median over roughly 1 MHz of the drawn trace, mixed 50/50 with the
        // raw trace: half-strength smoothing. It knocks the narrow clock
        // spurs down without erasing real narrow signals completely.
        const double range = static_cast<double>(stop_hz_ - start_hz_);
        const double bin_hz = range / std::max<size_t>(1, total_bins_);
        const double view_span =
            bin_hz * static_cast<double>(std::max<size_t>(1, view_bins_));
        const double hz_per_px = view_span / static_cast<double>(kWidth);
        int window = hz_per_px > 0.0
                         ? static_cast<int>(std::lround(1.0e6 / hz_per_px))
                         : 3;
        window = std::clamp(window, 3, 31);
        const std::vector<float> raw_row = row_db_;
        const std::vector<float> raw_hold = hold_row_db_;
        smoothTrace(row_db_, window);
        smoothTrace(hold_row_db_, window);
        for (int x = 0; x < kWidth; ++x) {
            row_db_[x] = 0.5f * (row_db_[x] + raw_row[x]);
            hold_row_db_[x] = 0.5f * (hold_row_db_[x] + raw_hold[x]);
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
    }

    // The max-hold trace is an envelope, so it only gets the spatial pass.
    std::vector<float> hold_smoothed(kWidth, -120.0f);
    if (hold_active) {
        for (int x = 0; x < kWidth; ++x) {
            const float left = hold_row_db_[x > 0 ? x - 1 : 0];
            const float right =
                hold_row_db_[x + 1 < kWidth ? x + 1 : kWidth - 1];
            hold_smoothed[x] = (left + 2.0f * hold_row_db_[x] + right) * 0.25f;
        }
    }

    // Vertical scale: track the median noise floor and keep a fixed 55-unit
    // window. A single strong peak (often at the right edge) can no longer
    // move the whole trace up and down.
    {
        std::vector<float> valid;
        valid.reserve(kWidth);
        for (int x = 0; x < kWidth; ++x) {
            if (row_db_[x] > -119.0f) valid.push_back(row_db_[x]);
        }
        if (!valid.empty()) {
            const size_t idx = valid.size() / 2;
            std::nth_element(valid.begin(), valid.begin() + idx,
                             valid.end());
            const float target = valid[idx] - 10.0f;
            if (!scale_initialized_) {
                // Lock the scale on the first frame instead of sliding in
                // from -100 dB over several seconds.
                floor_db_ = target;
                scale_initialized_ = true;
            } else {
                floor_db_ = 0.9f * floor_db_ + 0.1f * target;
            }
        }
    }
    ceil_db_ = floor_db_ + 55.0f;

    // Grid.
    for (int i = 1; i <= 3; ++i) {
        const int y = (i * kSpectrumH) / 4;
        for (int x = 0; x < kWidth; x += 4) {
            fb_.drawPixel(x, y, COLOR_GRID);
            fb_.drawPixel(x + 1, y, COLOR_GRID);
        }
    }

    auto y_for = [&](float value) {
        const float span = ceil_db_ - floor_db_;
        float norm = (value - floor_db_) / span;
        norm = std::clamp(norm, 0.0f, 1.0f);
        return kSpectrumH - 1 -
               static_cast<int>(norm * static_cast<float>(kSpectrumH - 3));
    };
    auto draw_curve = [&](const std::vector<float>& values, uint16_t color) {
        int prev_x = 0;
        int prev_y = y_for(values[0]);
        for (int x = 1; x < kWidth; ++x) {
            const int y = y_for(values[x]);
            fb_.drawLine(prev_x, prev_y, x, y, color);
            prev_x = x;
            prev_y = y;
        }
    };

    if (hold_active) draw_curve(hold_smoothed, COLOR_RED);
    draw_curve(row_db_, 0xFFE0);
    drawPeakLabels();
}

void SpectrumPage::drawPeakLabels() {
    if (!peak_labels_) return;

    struct Peak {
        int x;
        int y;
        float value;
    };
    std::vector<Peak> peaks;
    const float span_db = std::max(1.0f, ceil_db_ - floor_db_);
    const auto y_for = [&](float value) {
        const float norm =
            std::clamp((value - floor_db_) / span_db, 0.0f, 1.0f);
        return kSpectrumH - 1 -
               static_cast<int>(norm * static_cast<float>(kSpectrumH - 3));
    };

    for (int x = 6; x < kWidth - 6; ++x) {
        const float value = row_db_[x];
        if (value < floor_db_ + 15.0f) continue;
        if (value <= row_db_[x - 1] || value < row_db_[x + 1]) continue;
        float neighbour = 1e9f;
        for (int k = -6; k <= 6; ++k) {
            neighbour = std::min(
                neighbour, row_db_[std::clamp(x + k, 0, kWidth - 1)]);
        }
        if (value - neighbour < 8.0f) continue;
        peaks.push_back({x, y_for(value), value});
    }
    if (peaks.empty()) return;

    std::sort(peaks.begin(), peaks.end(),
              [](const Peak& a, const Peak& b) {
                  return a.value > b.value;
              });

    const double range = static_cast<double>(stop_hz_ - start_hz_);
    const double bin_hz =
        range / static_cast<double>(std::max<size_t>(1, total_bins_));
    const double view_span =
        bin_hz * static_cast<double>(std::max<size_t>(1, view_bins_));
    const double hz_per_px = view_span / static_cast<double>(kWidth);
    int decimals = 1;
    if (hz_per_px > 200000.0) {
        decimals = 1;
    } else if (hz_per_px > 20000.0) {
        decimals = 2;
    } else {
        decimals = 3;
    }

    struct Box {
        int x0;
        int x1;
    };
    std::vector<Box> used;
    int drawn = 0;
    for (const Peak& peak : peaks) {
        if (drawn >= 3) break;

        // Use the strongest raw bin inside this screen column for the label.
        size_t bin = view_start_ +
                     (static_cast<size_t>(peak.x) * view_bins_) / kWidth;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            if (bin >= bins_db_.size() && !bins_db_.empty()) {
                bin = bins_db_.size() - 1;
            }
            size_t end = view_start_ +
                         (static_cast<size_t>(peak.x + 1) * view_bins_) /
                             kWidth;
            if (end <= bin) end = bin + 1;
            end = std::min(end, bins_db_.size());
            for (size_t i = bin + 1; i < end; ++i) {
                if (bins_db_[i] > bins_db_[bin]) bin = i;
            }
        }

        const uint32_t hz = start_hz_ +
            static_cast<uint32_t>(std::llround(bin * bin_hz));
        const std::string label = formatMhz(hz, decimals);
        const int label_w = static_cast<int>(label.size()) * 7;
        int lx = peak.x - label_w / 2;
        lx = std::clamp(lx, 1, kWidth - label_w - 1);

        bool overlaps = false;
        for (const Box& box : used) {
            if (lx < box.x1 + 4 && lx + label_w + 4 > box.x0) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) continue;

        used.push_back({lx, lx + label_w});
        int ly = peak.y - 11;
        if (ly < 1) ly = 1;

        fb_.fillRect(lx - 1, ly - 1, label_w + 2, 10, 0x0000);
        ui_.drawText(lx, ly, label.c_str(), 0xFFE0);
        for (int y = ly + 10; y < peak.y - 1; ++y) {
            fb_.drawPixel(peak.x, y, 0x8410);
        }
        fb_.fillRect(peak.x, peak.y, 1, 2, 0xFFFF);
        ++drawn;
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

void SpectrumPage::onVolumeChanged(int percent) {
    char buf[16];
    if (percent >= 0) {
        std::snprintf(buf, sizeof(buf), "VOL %d%%", percent);
    } else {
        std::snprintf(buf, sizeof(buf), "NO VOL");
    }
    toast_ = buf;
    toast_until_ = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(1500);
}

void SpectrumPage::drawStatusBar() {
    // 70% transparent (alpha 0.3) so the page/waterfall behind the bar shows
    // through instead of a solid strip.
    for (int y = kStatusY; y < 170; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            fb_.blendPixel(x, y, COLOR_STATUSBAR, 0.3f);
        }
    }

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

    if (smooth_display_) {
        ui_.drawText(120, kStatusY + 3, "SM", COLOR_YELLOW);
    }

    if (!toast_.empty() &&
        std::chrono::steady_clock::now() < toast_until_) {
        ui_.drawText(178, kStatusY + 3, toast_.c_str(), COLOR_YELLOW);
    } else {
        char span_buf[24];
        std::snprintf(span_buf, sizeof(span_buf), "SPAN %.2fM",
                      view_span / 1e6);
        ui_.drawText(178, kStatusY + 3, span_buf, COLOR_TEXT_DIM);
    }

    if (max_hold_active_) {
        ui_.drawText(150, kStatusY + 3, "MAX", COLOR_RED);
    }

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
