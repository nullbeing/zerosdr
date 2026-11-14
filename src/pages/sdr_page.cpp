#include "pages/sdr_page.h"
#include "core/first_run_guide.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>

// Static member initialization
const uint32_t SDRPage::sample_rates_[] = {1024000, 1200000, 1800000, 2048000, 2400000};
const int SDRPage::num_sample_rates_ = 5;

const size_t SDRPage::fft_sizes_[] = {256, 512, 1024, 2048};
const int SDRPage::num_fft_sizes_ = 4;

const uint32_t SDRPage::display_bw_[] = {50000, 100000, 200000, 500000, 1000000, 2000000, 2800000, 3200000};
const int SDRPage::num_display_bw_ = 8;

const uint32_t SDRPage::fm_bandwidths_[] = {200000, 250000, 300000, 400000, 500000};
const uint32_t SDRPage::nfm_bandwidths_[] = {6250, 12500, 25000};
const uint32_t SDRPage::am_bandwidths_[] = {5000, 10000, 15000};
const uint32_t SDRPage::ssb_bandwidths_[] = {2400, 2700, 3000};

const char* SDRPage::demod_names_[] = {"FM", "NFM", "AM", "USB", "LSB"};

// Explicit step sizes (Hz). Index 0 is reserved for "Auto" (see getCurrentStep).
// The T key cycles through these in order, wrapping back to Auto.
const uint32_t SDRPage::step_sizes_[] = {0, 100, 1000, 5000, 10000, 100000, 1000000};
const int SDRPage::num_step_sizes_ = 7;

SDRPage::SDRPage(Framebuffer& fb, SDRController& sdr, AudioOutput& audio,
                 SDRUIRenderer& ui, InputHandler& input)
    : fb_(fb), sdr_(sdr), audio_(audio), ui_(ui), input_(input),
      fft_(512),
      demod_(1024000, 48000),
      center_freq_(100000000),
      sample_rate_(1024000),
      gain_(-1),
      volume_(1.0f),
      squelch_(0.0f),
      demod_mode_(0),
      audio_enabled_(true),
      audio_thread_running_(false),
      rate_index_(0),
      fft_index_(1),
      bw_index_(4),
      display_span_(1000000),
      fm_bw_index_(2),
      nfm_bw_index_(1),
      am_bw_index_(1),
      ssb_bw_index_(1),
      freq_input_len_(0),
      deemphasis_tau_us_(50.0f),
      step_index_(0),  // 0 = Auto
      squelch_values_{0.0f, 0.0f, 0.0f, 0.0f, 0.0f},  // FM/NFM/AM/USB/LSB
      frame_accumulator_(0.0f) {
    memset(freq_input_buffer_, 0, sizeof(freq_input_buffer_));
    // Pre-allocate audio output buffer (~200ms at 48kHz = 9600 samples)
    audio_samples_.reserve(9600);
}

SDRPage::~SDRPage() {
    stopAudioThread();
}

bool SDRPage::init() {
    std::cout << "SDRPage: Initializing..." << std::endl;
    loadSettings();
    system_volume_ok_ = system_volume_.init();
    int system_percent = 0;
    if (system_volume_ok_ && !system_volume_.read(system_percent)) {
        system_volume_ok_ = false;
    }
    // The sink already applies the volume, so the demod path stays at unity
    // gain instead of attenuating twice.
    demod_.setVolume(1.0f);
    if (system_volume_ok_) {
        volume_ = static_cast<float>(system_percent) / 100.0f;
        std::cout << "System volume: " << system_percent << "%"
                  << std::endl;
    } else {
        std::cout << "System volume unavailable (" << system_volume_.error()
                  << ")" << std::endl;
    }
    demod_.setSquelch(squelch_);
    demod_.setMode(AudioDemodulator::MODE_FM);
    demod_.setDeemphasisTau(deemphasis_tau_us_);
    demod_.setAudioAGC(audio_agc_);
    demod_.setDemodBandwidth(fm_bandwidths_[fm_bw_index_]);
    fft_.setFFTSize(fft_sizes_[fft_index_]);
    setSpanToSampleRate();
    std::cout << "SDR settings: deemphasis " << (int)deemphasis_tau_us_
              << " us, audio AGC " << (audio_agc_ ? "on" : "off")
              << ", FFT " << fft_sizes_[fft_index_]
              << ", sample rate " << sample_rates_[rate_index_] << " Hz"
              << std::endl;
    std::cout << "SDRPage: Initialized" << std::endl;
    return true;
}

void SDRPage::cleanup() {
    std::cout << "SDRPage: Cleaning up..." << std::endl;
    stopAudioThread();
}

void SDRPage::onEnter() {
    std::cout << "SDRPage: Entering SDR mode" << std::endl;
    // The launcher may have changed the system volume while another page was
    // active, so pick it up again here.
    refreshSystemVolume();

    // Check if SDR is already initialized and capturing
    // If so, we're coming from startup or another SDR-using page
    if (sdr_.isCapturing()) {
        std::cout << "SDRPage: SDR already capturing, no reinitialization needed" << std::endl;
        // Sync our state from the SDR's current configuration (preserves CLI args)
        center_freq_ = sdr_.getFrequency();
        sample_rate_ = sdr_.getSampleRate();
        // Restore the saved sample rate unless the command line overrode it
        // (1024000 is the built-in default used by main()).
        if (saved_sample_rate_ != 0 && sample_rate_ == 1024000 &&
            saved_sample_rate_ != sample_rate_) {
            sample_rate_ = saved_sample_rate_;
            sdr_.setSampleRate(sample_rate_);
            std::lock_guard<std::mutex> lock(demod_mutex_);
            demod_.setSampleRate(sample_rate_);
        }
        // Keep the zoom inside the captured band (preserve the user's choice
        // when it still fits).
        if (display_span_ > sample_rate_) {
            setSpanToSampleRate();
        }
        startAudioThread();
        fb_.clear(COLOR_BG_DARK);
        fb_.update();
        return;
    }

    // SDR is not capturing, we're likely coming from ADSB/Radar page
    // Kill any running readsb process to free up the SDR device
    std::cout << "SDRPage: Killing readsb process..." << std::endl;
    system("pkill -9 readsb 2>/dev/null");

    // Wait longer for USB device to be fully released
    // USB device release can take time after process termination
    usleep(1000000);  // 1 second delay

    // Try multiple times to reinitialize if device is still busy
    bool sdr_initialized = false;
    for (int retry = 0; retry < 3 && !sdr_initialized; retry++) {
        if (retry > 0) {
            std::cout << "SDRPage: Retry " << retry << " to initialize SDR device..." << std::endl;
            usleep(500000);  // Additional 500ms between retries
        }

        if (sdr_.init(0)) {
            sdr_initialized = true;
            std::cout << "SDRPage: Successfully reinitialized SDR device" << std::endl;
        }
    }

    if (!sdr_initialized) {
        std::cerr << "SDRPage: Failed to reinitialize SDR device after retries" << std::endl;
        // Show error on screen
        fb_.clear(COLOR_BG_DARK);
        ui_.drawText(10, 80, "SDR Device Busy", COLOR_RED);
        ui_.drawText(10, 100, "Please wait and try again", COLOR_TEXT_DIM);
        fb_.update();
        return;
    }

    sdr_.setFrequency(center_freq_);
    sdr_.setSampleRate(sample_rate_);
    if (gain_ >= 0) {
        sdr_.setAutoGain(false);
        sdr_.setGain(gain_);
        sdr_.setAGCMode(false);
    } else {
        sdr_.setAutoGain(true);
        sdr_.setAGCMode(true);
    }
    if (!sdr_.isCapturing()) {
        sdr_.startCapture();
        std::cout << "SDRPage: Started SDR capture" << std::endl;
    }
    startAudioThread();
    fb_.clear(COLOR_BG_DARK);
    fb_.update();
}

void SDRPage::onExit() {
    std::cout << "SDRPage: Exiting SDR mode" << std::endl;
    stopAudioThread();
}

void SDRPage::update(float delta_time) {
    // The launcher owns the volume keys; keep the displayed value in step
    // with the system sink.
    volume_poll_accum_ += delta_time;
    if (system_volume_ok_ && volume_poll_accum_ >= 0.5f) {
        volume_poll_accum_ = 0.0f;
        refreshSystemVolume();
    }

    // Audio is handled continuously by audioLoop(). The UI only needs a
    // non-consuming snapshot of the newest samples for the FFT.
    frame_accumulator_ += delta_time;
    if (frame_accumulator_ >= FRAME_INTERVAL_SEC) {
        frame_accumulator_ = 0.0f;
        size_t fft_bytes = fft_.getFFTSize() * 2;
        if (sdr_.getLatestSamplesSnapshot(fft_samples_, fft_bytes)) {
            fft_.process(fft_samples_.data(), fft_bytes, magnitude_);
            renderFrame();
        }
    }
}

void SDRPage::startAudioThread() {
    if (audio_thread_running_.load()) {
        return;
    }
    audio_thread_running_ = true;
    audio_thread_ = std::thread(&SDRPage::audioLoop, this);
}

void SDRPage::stopAudioThread() {
    if (!audio_thread_running_.load()) {
        return;
    }
    audio_thread_running_ = false;
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
}

void SDRPage::audioLoop() {
    // ~200 ms per chunk. The audio thread processes a chunk faster than
    // real-time, which keeps the ALSA buffer filled with a comfortable
    // cushion for UI/FFT and scheduler jitter. The DSP state is continuous
    // across chunks, so this does not introduce boundary clicks.
    std::vector<uint8_t> iq_samples;
    std::vector<int16_t> pcm_samples;
    uint64_t chunks = 0;
    uint64_t last_dropped = 0;
    auto last_success = std::chrono::steady_clock::now();

    while (audio_thread_running_.load()) {
        size_t chunk_samples = std::max<size_t>(4096, sdr_.getSampleRate() / 5);
        if (!sdr_.getLatestSamples(iq_samples, chunk_samples)) {
            usleep(2000);
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        double iq_gap_ms = std::chrono::duration<double, std::milli>(now - last_success).count();
        if (iq_gap_ms > 400.0) {
            std::cerr << "IQ gap " << (int)iq_gap_ms << " ms" << std::endl;
        }
        last_success = now;

        if ((++chunks % 25) == 0) {
            uint64_t dropped = sdr_.getDroppedBytes();
            if (dropped != last_dropped) {
                std::cerr << "SDR ring dropped " << (dropped - last_dropped)
                          << " bytes" << std::endl;
                last_dropped = dropped;
            }
        }

        // Always drain the ring so it stays fresh, even when audio is muted.
        if (!audio_enabled_.load() || !audio_.isOpen()) {
            continue;
        }

        auto t0 = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(demod_mutex_);
            demod_.demodulate(iq_samples, pcm_samples);
        }
        auto t1 = std::chrono::steady_clock::now();

        if (!pcm_samples.empty()) {
            audio_.write(pcm_samples);
        }
        auto t2 = std::chrono::steady_clock::now();

        double dsp_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double write_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (dsp_ms > 300.0) {
            std::cerr << "Audio DSP " << (int)dsp_ms << " ms" << std::endl;
        }
        if (write_ms > 300.0) {
            std::cerr << "Audio write " << (int)write_ms << " ms" << std::endl;
        }
    }
}

bool SDRPage::handleInput(AppKeyCode key) {
    if (key == APPKEY_H) {
        help_visible_ = !help_visible_;
        help_tab_ = 0;
        settings_index_ = 0;
        return true;
    }
    if (help_visible_) {
        handleOverlayInput(key);
        return true;
    }
    if (key >= APPKEY_0 && key <= APPKEY_9) {
        handleFrequencyInput(key);
        return true;
    }
    if (key == APPKEY_DOT || key == APPKEY_BACKSPACE || key == APPKEY_ENTER || key == APPKEY_ESC) {
        handleFrequencyInput(key);
        return true;
    }
    if (key == APPKEY_LEFT || key == APPKEY_RIGHT || key == APPKEY_PAGEUP || key == APPKEY_PAGEDOWN ||
        key == APPKEY_UP || key == APPKEY_DOWN) {
        handleNavigationKeys(key);
        return true;
    }
    if (key == APPKEY_V || key == APPKEY_C || key == APPKEY_S ||
        key == APPKEY_SPACE || key == APPKEY_D) {
        handleAudioControls(key);
        return true;
    }
    if (key == APPKEY_M || key == APPKEY_G || key == APPKEY_RES || key == APPKEY_BW ||
        key == APPKEY_ZOOM || key == APPKEY_PLUS || key == APPKEY_MINUS || key == APPKEY_T) {
        handleModeControls(key);
        return true;
    }
    if (key == APPKEY_SCREENSHOT) {
        char filename[64];
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        snprintf(filename, sizeof(filename),
                 "/tmp/zerosdr_%04d%02d%02d_%02d%02d%02d.png",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
        if (fb_.saveScreenshot(filename)) {
            std::cout << "Screenshot saved: " << filename << std::endl;
        } else {
            std::cerr << "Screenshot failed" << std::endl;
        }
        return true;
    }
    return false;
}

uint32_t SDRPage::getCurrentDemodBandwidth() const {
    switch (demod_mode_) {
        case 0: return fm_bandwidths_[fm_bw_index_];
        case 1: return nfm_bandwidths_[nfm_bw_index_];
        case 2: return am_bandwidths_[am_bw_index_];
        case 3: return ssb_bandwidths_[ssb_bw_index_];  // USB
        case 4: return ssb_bandwidths_[ssb_bw_index_];  // LSB
        default: return 200000;
    }
}

uint32_t SDRPage::getAutoStep() const {
    // Sensible default per demod mode. Auto aims at the channel spacing
    // common for that mode; SSB gets a finer step than the demod bandwidth
    // so users can dial onto voice peaks.
    switch (demod_mode_) {
        case 0: return 100000;   // FM broadcast: 100 kHz channel spacing
        case 1: return 12500;    // NFM: 12.5 kHz channel
        case 2: return 5000;     // AM: 5 kHz
        case 3:                   // USB
        case 4: return 1000;     // LSB: 1 kHz (finer than 2.4 kHz demod bw)
        default: return 100000;
    }
}

int SDRPage::maxSpanIndex() const {
    int max_index = 0;
    for (int i = 0; i < num_display_bw_; ++i) {
        if (display_bw_[i] <= sample_rate_) {
            max_index = i;
        }
    }
    return max_index;
}

void SDRPage::setSpanToSampleRate() {
    bw_index_ = maxSpanIndex();
    display_span_ = display_bw_[bw_index_];
}

uint32_t SDRPage::getCurrentStep() const {
    if (step_index_ == 0) return getAutoStep();
    return step_sizes_[step_index_];
}

const char* SDRPage::getStepLabel() const {
    if (step_index_ == 0) return "AUTO";
    uint32_t hz = step_sizes_[step_index_];
    if (hz >= 1000000) return "1M";
    if (hz >= 100000)  return "100k";
    if (hz >= 10000)   return "10k";
    if (hz >= 5000)    return "5k";
    if (hz >= 1000)    return "1k";
    return "100";
}

void SDRPage::renderFrame() {
    fb_.fillRect(0, 0, 320, 77, COLOR_BG_DARK);
    uint32_t current_demod_bw = getCurrentDemodBandwidth();
    uint32_t sample_rate = sdr_.getSampleRate();

    // A zoom level wider than the current sample rate would stretch the
    // frequency axis beyond the data actually captured. Clamp the rendered
    // span so the axis labels, red center marker and waterfall agree.
    uint32_t effective_span = std::min(display_span_, sample_rate);

    ui_.renderSpectrum(magnitude_, sdr_.getFrequency(), effective_span, sample_rate,
                       demod_mode_, current_demod_bw);
    ui_.renderWaterfall(magnitude_, effective_span, sample_rate);

    bool squelch_open;
    {
        std::lock_guard<std::mutex> lock(demod_mutex_);
        squelch_open = demod_.isSquelchOpen();
    }

    const char* input_display = (freq_input_len_ > 0) ? freq_input_buffer_ : nullptr;
    ui_.renderStatusBar(sdr_.getFrequency(), effective_span, sdr_.getGain(), input_display,
                       demod_names_[demod_mode_], volume_, squelch_open,
                       squelch_, audio_enabled_, sdr_.getAGCEnabled(), getStepLabel());

    if (help_visible_) {
        if (help_tab_ == 0) {
            ui_.renderKeyHelp();
        } else {
            SdrSettingsView view{};
            view.deemphasis_us = static_cast<int>(deemphasis_tau_us_);
            view.audio_agc = audio_agc_;
            view.fft_size = static_cast<int>(fft_sizes_[fft_index_]);
            view.sample_rate = static_cast<int>(sdr_.getSampleRate());
            view.selected = settings_index_;
            ui_.renderSettings(view);
        }
    }
    fb_.update();
}

void SDRPage::handleFrequencyInput(AppKeyCode key) {
    if (key >= APPKEY_0 && key <= APPKEY_9) {
        if (freq_input_len_ < 15) {
            freq_input_buffer_[freq_input_len_++] = '0' + (key - APPKEY_0);
            freq_input_buffer_[freq_input_len_] = '\0';
        }
    } else if (key == APPKEY_DOT) {
        if (freq_input_len_ < 15) {
            freq_input_buffer_[freq_input_len_++] = '.';
            freq_input_buffer_[freq_input_len_] = '\0';
        }
    } else if (key == APPKEY_BACKSPACE) {
        if (freq_input_len_ > 0) {
            freq_input_buffer_[--freq_input_len_] = '\0';
        }
    } else if (key == APPKEY_ENTER && freq_input_len_ > 0) {
        float freq_mhz = atof(freq_input_buffer_);
        if (freq_mhz > 0 && freq_mhz < 2000) {
            center_freq_ = (uint32_t)(freq_mhz * 1e6);
            sdr_.setFrequency(center_freq_);
            std::cout << "Set frequency to " << freq_mhz << " MHz" << std::endl;
        }
        freq_input_len_ = 0;
        freq_input_buffer_[0] = '\0';
    } else if (key == APPKEY_ESC && freq_input_len_ > 0) {
        freq_input_len_ = 0;
        freq_input_buffer_[0] = '\0';
    }
}

void SDRPage::handleNavigationKeys(AppKeyCode key) {
    // Tuning: arrow = current step, shift+arrow = step * 10, PgUp/PgDn = 1 MHz
    uint32_t step = getCurrentStep();
    uint32_t coarse = step * 10;
    const uint32_t big_step = 1000000;  // PageUp/PageDown

    switch (key) {
        case APPKEY_RIGHT:
        case APPKEY_RIGHT_COARSE:
            {
                uint32_t delta = (key == APPKEY_RIGHT_COARSE) ? coarse : step;
                center_freq_ += delta;
                sdr_.setFrequency(center_freq_);
            }
            break;
        case APPKEY_LEFT:
        case APPKEY_LEFT_COARSE:
            {
                uint32_t delta = (key == APPKEY_LEFT_COARSE) ? coarse : step;
                if (center_freq_ > delta) center_freq_ -= delta;
                else center_freq_ = 0;
                sdr_.setFrequency(center_freq_);
            }
            break;
        case APPKEY_PAGEUP:
            center_freq_ += big_step;
            sdr_.setFrequency(center_freq_);
            break;
        case APPKEY_PAGEDOWN:
            if (center_freq_ > big_step) center_freq_ -= big_step;
            else center_freq_ = 0;
            sdr_.setFrequency(center_freq_);
            break;
        case APPKEY_UP:
            if (gain_ < 0) {
                gain_ = 0;
                sdr_.setAGCMode(false);
                sdr_.setGain(gain_);
            } else if (gain_ < 490) {
                gain_ += 10;
                sdr_.setGain(gain_);
            }
            break;
        case APPKEY_DOWN:
            if (gain_ < 0) {
                gain_ = 0;
                sdr_.setAGCMode(false);
                sdr_.setGain(gain_);
            } else if (gain_ > 0) {
                gain_ -= 10;
                sdr_.setGain(gain_);
            }
            break;
    }
}

void SDRPage::handleAudioControls(AppKeyCode key) {
    switch (key) {
        case APPKEY_V:
            audio_enabled_ = true;  // volume-up also unmutes
            applyVolumeStep(10);
            break;
        case APPKEY_C:
            applyVolumeStep(-10);
            break;
        case APPKEY_S: {
            // Hold-to-accelerate: short tap = fine (5%), medium hold = 10%, long hold = 25%
            int hold_ms = input_.getLastReleasedHoldDuration(APPKEY_S);
            float step = 0.05f;
            if (hold_ms > 1500)      step = 0.25f;
            else if (hold_ms > 500)  step = 0.1f;

            std::lock_guard<std::mutex> lock(demod_mutex_);
            squelch_ += step;
            if (squelch_ > 1.0f) squelch_ = 0.0f;
            squelch_values_[demod_mode_] = squelch_;
            demod_.setSquelch(squelch_);
            std::cout << "Squelch: " << (int)(squelch_ * 100) << "%"
                      << " (step " << (int)(step * 100) << "%, hold "
                      << hold_ms << "ms)" << std::endl;
            break;
        }
        case APPKEY_SPACE:
            audio_enabled_ = !audio_enabled_;
            std::cout << "Audio: " << (audio_enabled_ ? "ON" : "OFF") << std::endl;
            break;
        case APPKEY_D: {
            std::lock_guard<std::mutex> lock(demod_mutex_);
            int old_mode = demod_mode_;
            demod_mode_ = (demod_mode_ + 1) % 5;
            demod_.setMode((AudioDemodulator::DemodMode)demod_mode_);
            // Save old mode's squelch, load new mode's squelch
            squelch_values_[old_mode] = squelch_;
            squelch_ = squelch_values_[demod_mode_];
            demod_.setSquelch(squelch_);

            uint32_t new_bw = getCurrentDemodBandwidth();
            demod_.setDemodBandwidth(new_bw);
            std::cout << "Demod mode: " << demod_names_[demod_mode_]
                      << ", bandwidth: " << new_bw / 1000 << " kHz"
                      << ", squelch: " << (int)(squelch_ * 100) << "%" << std::endl;
            break;
        }
    }
}

void SDRPage::applyVolumeStep(int delta_percent) {
    if (system_volume_ok_) {
        const int current =
            static_cast<int>(std::lround(volume_ * 100.0f));
        const int next = std::clamp(current + delta_percent, 0, 100);
        if (system_volume_.write(next)) {
            volume_ = static_cast<float>(next) / 100.0f;
        }
    }
    std::cout << "Volume: " << static_cast<int>(std::lround(volume_ * 100.0f))
              << "%" << (system_volume_ok_ ? " (system)" : "") << std::endl;
}

void SDRPage::refreshSystemVolume() {
    if (!system_volume_ok_) return;
    int percent = 0;
    if (!system_volume_.read(percent)) return;
    volume_ = static_cast<float>(percent) / 100.0f;
}

void SDRPage::handleModeControls(AppKeyCode key) {
    switch (key) {
        case APPKEY_M:
            {
                int mode = (ui_.getMode() + 1) % 3;
                ui_.setMode(mode);
                fb_.clear(COLOR_BG_DARK);
            }
            break;
        case APPKEY_G:
            if (sdr_.getAGCEnabled()) {
                gain_ = 0;
                sdr_.setAGCMode(false);
                sdr_.setGain(gain_);
                std::cout << "AGC: OFF (manual gain " << gain_/10 << " dB)" << std::endl;
            } else {
                gain_ = -1;
                sdr_.setAGCMode(true);
                std::cout << "AGC: ON" << std::endl;
            }
            break;
        case APPKEY_RES:
            fft_index_ = (fft_index_ + 1) % num_fft_sizes_;
            fft_.setFFTSize(fft_sizes_[fft_index_]);
            std::cout << "FFT size: " << fft_sizes_[fft_index_] << std::endl;
            break;
        case APPKEY_BW:
            {
                switch (demod_mode_) {
                    case 0: fm_bw_index_ = (fm_bw_index_ + 1) % 5; break;
                    case 1: nfm_bw_index_ = (nfm_bw_index_ + 1) % 3; break;
                    case 2: am_bw_index_ = (am_bw_index_ + 1) % 3; break;
                    case 3: ssb_bw_index_ = (ssb_bw_index_ + 1) % 3; break;  // USB
                    case 4: ssb_bw_index_ = (ssb_bw_index_ + 1) % 3; break;  // LSB
                }
                uint32_t new_bw = getCurrentDemodBandwidth();
                {
                    std::lock_guard<std::mutex> lock(demod_mutex_);
                    demod_.setDemodBandwidth(new_bw);
                }
                std::cout << "Demod bandwidth: " << new_bw / 1000 << " kHz" << std::endl;
            }
            break;
        case APPKEY_ZOOM: {
            // Only cycle through spans the current sample rate can actually
            // fill; wider levels would be clamped and appear dead.
            int max_index = maxSpanIndex();
            if (bw_index_ <= 0 || bw_index_ > max_index) {
                bw_index_ = max_index;
            } else {
                --bw_index_;
            }
            display_span_ = display_bw_[bw_index_];
            std::cout << "Display zoom: " << display_span_ / 1000 << " kHz"
                      << " (sample rate " << sample_rate_ / 1000 << " kHz)"
                      << std::endl;
            break;
        }
        case APPKEY_PLUS:
            rate_index_ = (rate_index_ + 1) % num_sample_rates_;
            sample_rate_ = sample_rates_[rate_index_];
            sdr_.setSampleRate(sample_rate_);
            {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                demod_.setSampleRate(sample_rate_);
            }
            break;
        case APPKEY_MINUS:
            rate_index_ = (rate_index_ + num_sample_rates_ - 1) % num_sample_rates_;
            sample_rate_ = sample_rates_[rate_index_];
            sdr_.setSampleRate(sample_rate_);
            {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                demod_.setSampleRate(sample_rate_);
            }
            break;
        case APPKEY_T:
            // Cycle tuning step: Auto -> 100 -> 1k -> 5k -> 10k -> 100k -> 1M -> Auto
            step_index_ = (step_index_ + 1) % num_step_sizes_;
            std::cout << "Tuning step: " << getStepLabel()
                      << " (" << getCurrentStep() << " Hz)" << std::endl;
            break;
    }
}

// ---------------------------------------------------------------------------
// Help / settings overlay
// ---------------------------------------------------------------------------

static std::string sdrSettingsPath() {
    const char* home = getenv("HOME");
    return std::string(home ? home : "/home/pi") + "/.config/zerosdr/sdr_settings.json";
}

static void ensureParentDirs(const std::string& path) {
    std::string dir;
    for (size_t i = 0; i < path.size(); ++i) {
        dir += path[i];
        if (path[i] == '/' && i + 1 < path.size()) {
            mkdir(dir.c_str(), 0755);
        }
    }
}

void SDRPage::handleOverlayInput(AppKeyCode key) {
    // Left/Right switches between the key list and the settings tab.
    if (key == APPKEY_LEFT || key == APPKEY_RIGHT) {
        help_tab_ = (help_tab_ + 1) % 2;
        settings_index_ = 0;
        return;
    }
    if (help_tab_ != 1) {
        return;
    }
    if (key == APPKEY_UP) {
        settings_index_ = (settings_index_ + 4) % 5;
    } else if (key == APPKEY_DOWN) {
        settings_index_ = (settings_index_ + 1) % 5;
    } else if (key == APPKEY_ENTER) {
        cycleSetting(settings_index_);
    }
}

void SDRPage::cycleSetting(int index) {
    switch (index) {
        case 0:
            deemphasis_tau_us_ = (deemphasis_tau_us_ > 60.0f) ? 50.0f : 75.0f;
            demod_.setDeemphasisTau(deemphasis_tau_us_);
            std::cout << "FM deemphasis: " << (int)deemphasis_tau_us_ << " us"
                      << std::endl;
            break;
        case 1:
            audio_agc_ = !audio_agc_;
            demod_.setAudioAGC(audio_agc_);
            std::cout << "Audio AGC: " << (audio_agc_ ? "ON" : "OFF") << std::endl;
            break;
        case 2:
            fft_index_ = (fft_index_ + 1) % num_fft_sizes_;
            fft_.setFFTSize(fft_sizes_[fft_index_]);
            std::cout << "FFT size: " << fft_sizes_[fft_index_] << std::endl;
            break;
        case 4:
            // Open the guide right away instead of only arming it for the
            // next start.
            help_visible_ = false;
            runFirstRunGuide(fb_, input_);
            break;
        case 3:
            rate_index_ = (rate_index_ + 1) % num_sample_rates_;
            sample_rate_ = sample_rates_[rate_index_];
            sdr_.setSampleRate(sample_rate_);
            {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                demod_.setSampleRate(sample_rate_);
            }
            // The old zoom may not fit the new band; show the full band.
            setSpanToSampleRate();
            std::cout << "Sample rate: " << sample_rate_ << " Hz, span "
                      << display_span_ / 1000 << " kHz" << std::endl;
            break;
        default:
            return;
    }
    saveSettings();
}

void SDRPage::loadSettings() {
    const std::string path = sdrSettingsPath();
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();

    auto int_value = [&](const char* key, long fallback) -> long {
        const std::string quoted = std::string("\"") + key + "\"";
        size_t pos = text.find(quoted);
        if (pos == std::string::npos) return fallback;
        pos = text.find(':', pos);
        if (pos == std::string::npos) return fallback;
        return strtol(text.c_str() + pos + 1, nullptr, 10);
    };
    auto bool_value = [&](const char* key, bool fallback) -> bool {
        const std::string quoted = std::string("\"") + key + "\"";
        size_t pos = text.find(quoted);
        if (pos == std::string::npos) return fallback;
        pos = text.find(':', pos);
        if (pos == std::string::npos) return fallback;
        const size_t t = text.find("true", pos);
        const size_t f = text.find("false", pos);
        if (t != std::string::npos && (f == std::string::npos || t < f)) return true;
        if (f != std::string::npos) return false;
        return fallback;
    };

    deemphasis_tau_us_ = (int_value("deemphasis_us", 50) == 75) ? 75.0f : 50.0f;
    audio_agc_ = bool_value("audio_agc", true);

    const long fft = int_value("fft_size", 1024);
    for (int i = 0; i < num_fft_sizes_; ++i) {
        if (static_cast<long>(fft_sizes_[i]) == fft) { fft_index_ = i; break; }
    }
    const long rate = int_value("sample_rate", 0);
    for (int i = 0; i < num_sample_rates_; ++i) {
        if (static_cast<long>(sample_rates_[i]) == rate) {
            rate_index_ = i;
            saved_sample_rate_ = sample_rates_[i];
            break;
        }
    }
    std::cout << "SDR settings: loaded " << path << std::endl;
}

void SDRPage::saveSettings() const {
    const std::string path = sdrSettingsPath();
    ensureParentDirs(path);
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "SDR settings: cannot write " << path << std::endl;
        return;
    }
    file << "{\n"
         << "  \"deemphasis_us\": " << (int)deemphasis_tau_us_ << ",\n"
         << "  \"audio_agc\": " << (audio_agc_ ? "true" : "false") << ",\n"
         << "  \"fft_size\": " << fft_sizes_[fft_index_] << ",\n"
         << "  \"sample_rate\": " << sample_rates_[rate_index_] << "\n"
         << "}\n";
    file.close();
}
