#include "framebuffer.h"
#include "sdr_controller.h"
#include "fft_processor.h"
#include "ui_renderer.h"
#include "input_handler.h"
#include "audio_demodulator.h"
#include "audio_output.h"
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <cstring>
#include <ctime>

static volatile bool running = true;

void signal_handler(int signum) {
    std::cerr << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    running = false;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -f <freq>     Set center frequency in Hz (default: 100000000)\n"
              << "  -s <rate>     Set sample rate in Hz (default: 2048000)\n"
              << "  -g <gain>     Set gain in dB (default: auto)\n"
              << "  -d <device>   Set framebuffer device (default: /dev/fb0)\n"
              << "  -k <device>   Set keyboard input device (default: auto-detect)\n"
              << "  -h            Show this help\n";
}

int main(int argc, char* argv[]) {
    // Default parameters
    uint32_t center_freq = 100000000; // 100 MHz
    uint32_t sample_rate = 1024000;   // 1.024 MHz (better for audio)
    int gain = -1;                     // Auto gain
    const char* fb_device = "/dev/fb0";
    const char* kbd_device = nullptr;  // Auto-detect

    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "f:s:g:d:k:h")) != -1) {
        switch (opt) {
            case 'f':
                center_freq = atoi(optarg);
                break;
            case 's':
                sample_rate = atoi(optarg);
                break;
            case 'g':
                gain = atoi(optarg);
                break;
            case 'd':
                fb_device = optarg;
                break;
            case 'k':
                kbd_device = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "zeroSDR - Initializing..." << std::endl;

    // Initialize framebuffer
    Framebuffer fb(fb_device);
    if (!fb.init()) {
        std::cerr << "Failed to initialize framebuffer" << std::endl;
        return 1;
    }
    std::cout << "Framebuffer initialized: " << fb.getWidth() << "x" << fb.getHeight() << std::endl;

    // Initialize SDR controller
    SDRController sdr;
    if (!sdr.init(0)) {
        std::cerr << "Failed to initialize RTL-SDR device" << std::endl;
        return 1;
    }
    std::cout << "RTL-SDR device initialized" << std::endl;

    // Configure SDR
    if (!sdr.setFrequency(center_freq)) {
        std::cerr << "Failed to set frequency" << std::endl;
        return 1;
    }

    if (!sdr.setSampleRate(sample_rate)) {
        std::cerr << "Failed to set sample rate" << std::endl;
        return 1;
    }

    if (gain >= 0) {
        sdr.setAutoGain(false);
        sdr.setGain(gain);
    } else {
        sdr.setAutoGain(true);
    }

    // Enable hardware AGC by default
    sdr.setAGCMode(true);
    std::cout << "Hardware AGC: enabled" << std::endl;

    std::cout << "Frequency: " << center_freq << " Hz" << std::endl;
    std::cout << "Sample rate: " << sample_rate << " Hz" << std::endl;
    std::cout << "Gain: " << (gain >= 0 ? std::to_string(gain) : "auto") << std::endl;

    // Initialize FFT processor
    FFTProcessor fft(512);

    // Initialize UI renderer
    UIRenderer ui(fb);

    // Initialize keyboard input
    InputHandler input;
    if (!input.init(kbd_device)) {
        std::cerr << "Warning: keyboard input disabled" << std::endl;
    }

    // Initialize audio components
    AudioDemodulator demod(sample_rate, 48000);
    AudioOutput audio_output;
    bool audio_enabled = true;  // Audio starts enabled
    if (!audio_output.init("default", 48000, 1)) {
        std::cerr << "Warning: audio output disabled" << std::endl;
        audio_enabled = false;  // Disable if init fails
    } else {
        std::cout << "Audio output initialized" << std::endl;
    }

    // Audio control variables
    float volume = 0.5f;
    float squelch = 0.0f;  // 0 = disabled (no squelch)
    int demod_mode = 0;  // 0=FM, 1=NFM, 2=AM
    const char* demod_names[] = {"FM", "NFM", "AM"};
    demod.setVolume(volume);
    demod.setSquelch(squelch);
    demod.setMode(AudioDemodulator::MODE_FM);

    // Sample rate presets (RTL-SDR max stable: 2.8MHz, theo: 3.2MHz)
    // Lower rates are better for audio quality (less resampling)
    const uint32_t sample_rates[] = {1024000, 1200000, 1800000, 2048000, 2400000};
    int rate_index = 0;  // Default 1.024M for better audio quality

    // FFT size presets (resolution/bandwidth)
    const size_t fft_sizes[] = {256, 512, 1024, 2048};
    int fft_index = 1;  // Default 512

    // Display bandwidth presets (zoom levels) — from narrow to ultra-wide
    const uint32_t display_bw[] = {50000, 100000, 200000, 500000, 1000000, 2000000, 2800000, 3200000};
    int bw_index = 7;  // Default: full 3.2MHz
    uint32_t display_span = display_bw[bw_index];

    // Demodulation bandwidth presets for each mode
    const uint32_t fm_bandwidths[] = {200000, 250000, 300000, 400000, 500000};    // FM: 200k, 250k, 300k, 400k, 500k
    const uint32_t nfm_bandwidths[] = {6250, 12500, 25000};       // NFM: 6.25k, 12.5k, 25k
    const uint32_t am_bandwidths[] = {5000, 10000, 15000};        // AM: 5k, 10k, 15k

    int fm_bw_index = 2;   // Default: 300kHz (better margin)
    int nfm_bw_index = 1;  // Default: 12.5kHz
    int am_bw_index = 1;   // Default: 10kHz

    // Helper function to get current demodulation bandwidth
    auto getCurrentDemodBandwidth = [&]() -> uint32_t {
        switch (demod_mode) {
            case 0: return fm_bandwidths[fm_bw_index];
            case 1: return nfm_bandwidths[nfm_bw_index];
            case 2: return am_bandwidths[am_bw_index];
            default: return 200000;
        }
    };

    // Start capture
    if (!sdr.startCapture()) {
        std::cerr << "Failed to start capture" << std::endl;
        return 1;
    }

    std::cout << "Starting main loop..." << std::endl;
    fb.clear(COLOR_BG_DARK);

    std::vector<uint8_t> samples;
    std::vector<float> magnitude;

    // Frequency input buffer
    char freq_input_buffer[16] = {0};
    int freq_input_len = 0;

    // Main loop
    while (running) {
        // Get latest samples (use much larger buffer for audio to avoid underruns)
        // For 48kHz audio at 50ms intervals, we need: 48000 * 0.05 = 2400 samples
        // At 2.048MHz SDR rate: 2400 * (2048000/48000) ≈ 102400 IQ samples
        // Double it for safety margin: 204800 bytes = 102400 complex samples = 50ms
        size_t sample_count = audio_enabled ? 204800 : fft.getFFTSize() * 2;
        if (sdr.getLatestSamples(samples, sample_count)) {
            // Process FFT (use first portion for spectrum display)
            std::vector<uint8_t> fft_samples(samples.begin(),
                                             samples.begin() + std::min(samples.size(), (size_t)(fft.getFFTSize() * 2)));
            fft.process(fft_samples, magnitude);

            // Clear only spectrum area (waterfall manages itself)
            fb.fillRect(0, 0, 320, 77, COLOR_BG_DARK);  // 清除频谱+频率坐标轴区域

            // Render all UI components
            uint32_t current_demod_bw = getCurrentDemodBandwidth();
            ui.renderSpectrum(magnitude, sdr.getFrequency(), display_span, sdr.getSampleRate(), demod_mode, current_demod_bw);
            ui.renderWaterfall(magnitude);

            // Show frequency input if active, otherwise normal status
            const char* input_display = (freq_input_len > 0) ? freq_input_buffer : nullptr;
            ui.renderStatusBar(sdr.getFrequency(), display_span, sdr.getGain(), input_display,
                               demod_names[demod_mode], volume, demod.isSquelchOpen(), audio_enabled,
                               sdr.getAGCEnabled());

            // Audio demodulation (use all samples for better audio quality)
            if (audio_enabled && audio_output.isOpen()) {
                std::vector<int16_t> audio_samples;
                demod.demodulate(samples, audio_samples);
                if (!audio_samples.empty()) {
                    audio_output.write(audio_samples);
                }
            }

            fb.update();
        }

        usleep(30000); // 30ms update rate (~33 FPS) - faster for smoother audio

        // Check if S key is held for long press reset (before polling new events)
        int s_hold_ms = input.getHoldDuration(APPKEY_S);
        if (s_hold_ms > 500) {
            // Long press detected - reset squelch immediately
            squelch = 0.0f;
            demod.setSquelch(squelch);
            std::cout << "Squelch: RESET (0%)" << std::endl;
            // Wait for key release to avoid repeated resets
            while (input.getHoldDuration(APPKEY_S) > 0) {
                usleep(10000);
                input.pollKey();  // Clear events
            }
        }

        // Handle keyboard input
        AppKeyCode key = input.pollKey();

        // Handle number input for frequency
        if (key >= APPKEY_0 && key <= APPKEY_9) {
            if (freq_input_len < 15) {
                freq_input_buffer[freq_input_len++] = '0' + (key - APPKEY_0);
                freq_input_buffer[freq_input_len] = '\0';
            }
            continue;
        }

        if (key == APPKEY_DOT) {
            if (freq_input_len < 15) {
                freq_input_buffer[freq_input_len++] = '.';
                freq_input_buffer[freq_input_len] = '\0';
            }
            continue;
        }

      if (key == APPKEY_BACKSPACE) {
            if (freq_input_len > 0) {
                freq_input_buffer[--freq_input_len] = '\0';
            }
            continue;
        }

        if (key == APPKEY_ENTER && freq_input_len > 0) {
            // Parse frequency input (in MHz)
            float freq_mhz = atof(freq_input_buffer);
            if (freq_mhz > 0 && freq_mhz < 2000) {  // Valid range: 0-2000 MHz
                center_freq = (uint32_t)(freq_mhz * 1e6);
                sdr.setFrequency(center_freq);
                std::cout << "Set frequency to " << freq_mhz << " MHz" << std::endl;
            }
            // Clear input buffer
            freq_input_len = 0;
            freq_input_buffer[0] = '\0';
            continue;
        }

        // ESC cancels frequency input
        if (key == APPKEY_ESC && freq_input_len > 0) {
            freq_input_len = 0;
            freq_input_buffer[0] = '\0';
            continue;
        }

        switch (key) {
            case APPKEY_RIGHT: {
                // Accelerate based on hold duration
                int hold_ms = input.getHoldDuration(APPKEY_RIGHT);
                int step = 100000;  // Base: 100 kHz
                if (hold_ms > 2000) step = 1000000;      // 1 MHz after 2s
                else if (hold_ms > 1000) step = 500000;  // 500 kHz after 1s
                else if (hold_ms > 500) step = 250000;   // 250 kHz after 0.5s
                center_freq += step;
                sdr.setFrequency(center_freq);
                break;
            }
            case APPKEY_LEFT: {
                int hold_ms = input.getHoldDuration(APPKEY_LEFT);
                int step = 100000;
                if (hold_ms > 2000) step = 1000000;
                else if (hold_ms > 1000) step = 500000;
                else if (hold_ms > 500) step = 250000;
                if (center_freq > step) center_freq -= step;
                sdr.setFrequency(center_freq);
                break;
            }
            case APPKEY_PAGEUP: {
                // Shift+Right: large steps with acceleration
                int hold_ms = input.getHoldDuration(APPKEY_PAGEUP);
                int step = 1000000;  // Base: 1 MHz
                if (hold_ms > 2000) step = 10000000;     // 10 MHz after 2s
                else if (hold_ms > 1000) step = 5000000; // 5 MHz after 1s
                else if (hold_ms > 500) step = 2000000;  // 2 MHz after 0.5s
                center_freq += step;
                sdr.setFrequency(center_freq);
                break;
            }
            case APPKEY_PAGEDOWN: {
                int hold_ms = input.getHoldDuration(APPKEY_PAGEDOWN);
                int step = 1000000;
                if (hold_ms > 2000) step = 10000000;
                else if (hold_ms > 1000) step = 5000000;
                else if (hold_ms > 500) step = 2000000;
                if (center_freq > step) center_freq -= step;
                sdr.setFrequency(center_freq);
                break;
            }
            case APPKEY_UP:
                if (gain < 0) {
                    gain = 0;
                    sdr.setAGCMode(false);  // Disable AGC when entering manual mode
                    sdr.setGain(gain);
                } else if (gain < 490) {
                    gain += 10;
                    sdr.setGain(gain);
                }
                break;
            case APPKEY_DOWN:
                if (gain < 0) {
                    gain = 0;
                    sdr.setAGCMode(false);  // Disable AGC when entering manual mode
                    sdr.setGain(gain);
                } else if (gain > 0) {
                    gain -= 10;
                    sdr.setGain(gain);
                }
                break;
            case APPKEY_PLUS:
                rate_index = (rate_index + 1) % 5;
                sample_rate = sample_rates[rate_index];
                sdr.setSampleRate(sample_rate);
                break;
            case APPKEY_MINUS:
                rate_index = (rate_index + 4) % 5;
                sample_rate = sample_rates[rate_index];
                sdr.setSampleRate(sample_rate);
                break;
            case APPKEY_M: {
                int mode = (ui.getMode() + 1) % 3;
                ui.setMode(mode);
                // Clear screen when switching modes
                fb.clear(COLOR_BG_DARK);
                break;
            }
            case APPKEY_A: {
                bool agc = !demod.getAudioAGC();
                demod.setAudioAGC(agc);
                std::cout << "Audio AGC: " << (agc ? "ON" : "OFF") << std::endl;
                break;
            }
            case APPKEY_G:
                if (sdr.getAGCEnabled()) {
                    // Turning AGC OFF - switch to manual gain
                    gain = 0;  // Start at 0 dB (less noise)
                    sdr.setAGCMode(false);
                    sdr.setGain(gain);
                    std::cout << "AGC: OFF (manual gain " << gain/10 << " dB)" << std::endl;
                } else {
                    // Turning AGC ON - enable auto gain
                    gain = -1;  // Mark as auto mode
                    sdr.setAGCMode(true);
                    std::cout << "AGC: ON" << std::endl;
                }
                break;
            case APPKEY_Q:
            case APPKEY_ESC:
                running = false;
                break;
            case APPKEY_RES:
                fft_index = (fft_index + 1) % 4;
                fft.setFFTSize(fft_sizes[fft_index]);
                std::cout << "FFT size: " << fft_sizes[fft_index] << std::endl;
                break;
            case APPKEY_BW: {
                // Cycle through demodulation bandwidth presets for current mode
                switch (demod_mode) {
                    case 0:  // FM
                        fm_bw_index = (fm_bw_index + 1) % 5;  // 5 FM bandwidth options
                        break;
                    case 1:  // NFM
                        nfm_bw_index = (nfm_bw_index + 1) % 3;
                        break;
                    case 2:  // AM
                        am_bw_index = (am_bw_index + 1) % 3;
                        break;
                }
                uint32_t new_bw = getCurrentDemodBandwidth();
                demod.setDemodBandwidth(new_bw);
                std::cout << "Demod bandwidth: " << new_bw / 1000 << " kHz" << std::endl;
                break;
            }
            case APPKEY_ZOOM:
                // Cycle through display bandwidth (zoom levels) - from wide to narrow
                bw_index = (bw_index + 7) % 8;  // Decrement: 7->6->5->...->0->7
                display_span = display_bw[bw_index];
                std::cout << "Display zoom: " << display_span / 1000 << " kHz" << std::endl;
                break;
            case APPKEY_SCREENSHOT: {
                // Generate filename with timestamp
                char filename[64];
                time_t now = time(nullptr);
                struct tm* t = localtime(&now);
                snprintf(filename, sizeof(filename),
                         "/home/edwin/zerosdr_%04d%02d%02d_%02d%02d%02d.png",
                         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                         t->tm_hour, t->tm_min, t->tm_sec);
                if (fb.saveScreenshot(filename)) {
                    std::cout << "Screenshot saved: " << filename << std::endl;
                } else {
                    std::cerr << "Screenshot failed" << std::endl;
                }
                break;
            }
            case APPKEY_D: {
                // Toggle demodulation mode (FM -> NFM -> AM -> FM)
                demod_mode = (demod_mode + 1) % 3;
                demod.setMode((AudioDemodulator::DemodMode)demod_mode);

                // Set default bandwidth for new mode
                uint32_t new_bw = getCurrentDemodBandwidth();
                demod.setDemodBandwidth(new_bw);

                std::cout << "Demod mode: " << demod_names[demod_mode]
                          << ", bandwidth: " << new_bw / 1000 << " kHz" << std::endl;
                break;
            }
            case APPKEY_V:
                // Volume up
                volume = std::min(1.0f, volume + 0.1f);
                demod.setVolume(volume);
                std::cout << "Volume: " << (int)(volume * 100) << "%" << std::endl;
                break;
            case APPKEY_C:
                // Volume down
                volume = std::max(0.0f, volume - 0.1f);
                demod.setVolume(volume);
                std::cout << "Volume: " << (int)(volume * 100) << "%" << std::endl;
                break;
            case APPKEY_S: {
                // Adjust squelch threshold
                // Only process on key release (short press increments)
                int hold_ms = input.getLastReleasedHoldDuration(APPKEY_S);
                if (hold_ms > 0 && hold_ms <= 500) {
                    // Short press: increment
                    squelch += 0.05f;
                    if (squelch > 1.0f) squelch = 0.0f;
                    std::cout << "Squelch: " << (int)(squelch * 100) << "%" << std::endl;
                    demod.setSquelch(squelch);
                }
                break;
            }
            case APPKEY_SPACE:
                // Toggle audio output
                audio_enabled = !audio_enabled;
                std::cout << "Audio: " << (audio_enabled ? "ON" : "OFF") << std::endl;
                break;
            default:
                break;
        }
    }

    // Cleanup
    std::cout << "Shutting down..." << std::endl;
    input.restore();
    sdr.stopCapture();
    sdr.close();
    fb.clear(COLOR_BG_DARK);
    fb.update();

    return 0;
}
