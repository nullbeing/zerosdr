#include "framebuffer.h"
#include "sdr_controller.h"
#include "fft_processor.h"
#include "ui_renderer.h"
#include "input_handler.h"
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
    uint32_t sample_rate = 2048000;   // 2.048 MHz
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

    // Sample rate presets (RTL-SDR max stable: 2.8MHz, theoretical: 3.2MHz)
    const uint32_t sample_rates[] = {1024000, 2048000, 2400000, 2800000, 3200000};
    int rate_index = 2;  // Default 2.4M

    // FFT size presets (resolution/bandwidth)
    const size_t fft_sizes[] = {256, 512, 1024, 2048};
    int fft_index = 1;  // Default 512

    // Display bandwidth presets (zoom levels) — from narrow to ultra-wide
    const uint32_t display_bw[] = {50000, 100000, 200000, 500000, 1000000, 2000000, 2800000, 3200000};
    int bw_index = 7;  // Default: full 3.2MHz
    uint32_t display_span = display_bw[bw_index];

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
        // Get latest samples
        if (sdr.getLatestSamples(samples, fft.getFFTSize() * 2)) {
            // Process FFT
            fft.process(samples, magnitude);

            // Clear only spectrum area (waterfall manages itself)
            fb.fillRect(0, 0, 320, 77, COLOR_BG_DARK);  // 清除频谱+频率坐标轴区域

            // Render all UI components
            ui.renderSpectrum(magnitude, sdr.getFrequency(), display_span, sdr.getSampleRate());
            ui.renderWaterfall(magnitude);

            // Show frequency input if active, otherwise normal status
            const char* input_display = (freq_input_len > 0) ? freq_input_buffer : nullptr;
            ui.renderStatusBar(sdr.getFrequency(), display_span, sdr.getGain(), input_display);

            fb.update();
        }

        usleep(50000); // 50ms update rate (~20 FPS)

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
                if (gain < 0) { gain = 0; sdr.setAutoGain(false); }
                else if (gain < 490) gain += 10;
                sdr.setGain(gain);
                break;
            case APPKEY_DOWN:
                if (gain < 0) { gain = 0; sdr.setAutoGain(false); }
                else if (gain > 0) gain -= 10;
                sdr.setGain(gain);
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
            case APPKEY_A:
                if (gain < 0) {
                    gain = 200;  // Start at 20 dB when leaving auto
                    sdr.setAutoGain(false);
                    sdr.setGain(gain);
                } else {
                    gain = -1;
                    sdr.setAutoGain(true);
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
            case APPKEY_BW:
                bw_index = (bw_index + 1) % 8;
                display_span = display_bw[bw_index];
                std::cout << "Display bandwidth: " << display_span / 1000 << " kHz" << std::endl;
                break;
            case APPKEY_SCREENSHOT: {
                // Generate filename with timestamp
                char filename[64];
                time_t now = time(nullptr);
                struct tm* t = localtime(&now);
                snprintf(filename, sizeof(filename),
                         "/home/edwin/zerosdr_%04d%02d%02d_%02d%02d%02d.ppm",
                         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                         t->tm_hour, t->tm_min, t->tm_sec);
                if (fb.saveScreenshot(filename)) {
                    std::cout << "Screenshot saved: " << filename << std::endl;
                } else {
                    std::cerr << "Screenshot failed" << std::endl;
                }
                break;
            }
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
