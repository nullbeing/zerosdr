#include "framebuffer.h"
#include "sdr_controller.h"
#include "ui/sdr_ui_renderer.h"
#include "ui/adsb_ui_renderer.h"
#include "ui/radar_ui_renderer.h"
#include "input_handler.h"
#include "audio_output.h"
#include "core/page_manager.h"
#include "core/first_run_guide.h"
#include "pages/sdr_page.h"
#include "pages/adsb_page.h"
#include "version.h"
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <cstring>
#include <ctime>

static volatile bool running = true;

static PageManager* g_page_manager = nullptr;

void signal_handler(int signum) {
    std::cerr << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    running = false;
    if (g_page_manager) {
        g_page_manager->stop();
    }
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -f <freq>     Set center frequency in Hz (default: 100000000)\n"
              << "  -s <rate>     Set sample rate in Hz (default: 1024000)\n"
              << "  -g <gain>     Set gain in dB (default: auto)\n"
              << "  -d <device>   Set framebuffer device (default: /dev/fb0)\n"
              << "  -k <device>   Set keyboard input device (default: auto-detect)\n"
              << "  -h            Show this help\n";
}

int main(int argc, char* argv[]) {
    // Default parameters
    uint32_t center_freq = 100000000; // 100 MHz
    uint32_t sample_rate = 1024000;   // 1.024 MHz
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

    std::cout << "zeroSDR " << ZEROSDR_VERSION_STRING << " - Initializing..." << std::endl;

    // Initialize framebuffer
    Framebuffer fb(fb_device);
    if (!fb.init()) {
        std::cerr << "Failed to initialize framebuffer" << std::endl;
        return 1;
    }
    std::cout << "Framebuffer initialized: " << fb.getWidth() << "x" << fb.getHeight() << std::endl;

    // Initialize UI renderers
    SDRUIRenderer sdr_ui(fb);
    ADSBUIRenderer adsb_ui(fb);
    RadarUIRenderer radar_ui(fb);

    // Initialize keyboard input
    InputHandler input;
    if (!input.init(kbd_device)) {
        std::cerr << "Warning: keyboard input disabled" << std::endl;
    }

    // Try to initialize SDR controller
    SDRController sdr;
    bool sdr_initialized = false;

    while (!sdr_initialized && running) {
        if (sdr.init(0)) {
            sdr_initialized = true;
            std::cout << "RTL-SDR device initialized" << std::endl;
            break;
        }

        // SDR initialization failed - show dialog with background UI
        std::cerr << "Failed to initialize RTL-SDR device" << std::endl;

        // Render background UI
        fb.clear(COLOR_BG_DARK);

        // Draw empty spectrum area with grid
        for (int i = 1; i <= 3; i++) {
            int grid_y = SPECTRUM_Y + (i * SPECTRUM_H) / 4;
            for (int gx = 0; gx < 320; gx += 4) {
                fb.drawPixel(gx, grid_y, COLOR_GRID);
                fb.drawPixel(gx + 1, grid_y, COLOR_GRID);
            }
        }

        // Draw zeroSDR logo
        sdr_ui.drawLogo(0, SPECTRUM_Y);

        // Draw red center frequency indicator
        int center_x = 320 / 2;
        for (int cy = SPECTRUM_Y; cy < SPECTRUM_Y + SPECTRUM_H; cy++) {
            fb.drawPixel(center_x, cy, COLOR_RED);
        }

        // Draw frequency axis
        sdr_ui.drawFreqAxis(center_freq, sample_rate, FREQAXIS_Y, FREQAXIS_H);

        // Draw status bar
        sdr_ui.renderStatusBar(center_freq, sample_rate, gain, nullptr, "--", 0.0f, false, 0.0f, false, false, "AUTO");

        // Draw dialog
        sdr_ui.renderDialog(
            "RTL-SDR Not Found",
            "Please connect RTL-SDR device",
            "R: Retry | Q: Quit"
        );
        fb.update();

        // Wait for use
        bool waiting = true;
        while (waiting && running) {
            usleep(50000);  // 50ms
            AppKeyCode key = input.pollKey();

            switch (key) {
                case APPKEY_R:
                    waiting = false;
                    std::cout << "Retrying RTL-SDR initialization..." << std::endl;
                    break;

                case APPKEY_SCREENSHOT: {
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

                case APPKEY_Q:
                case APPKEY_ESC:
                    std::cout << "User cancelled" << std::endl;
                    input.restore();
                    fb.clear(COLOR_BG_DARK);
                    fb.update();
                    return 0;

                default:
                    break;
            }
        }
    }

    if (!sdr_initialized) {
        std::cerr << "Failed to initialize RTL-SDR device" << std::endl;
        return 1;
    }

    // Configure SDR with command-line parameters
    sdr.setFrequency(center_freq);
    sdr.setSampleRate(sample_rate);
    if (gain >= 0) {
        sdr.setAutoGain(false);
        sdr.setGain(gain);
        sdr.setAGCMode(false);
    } else {
        sdr.setAutoGain(true);
        sdr.setAGCMode(true);
    }

    std::cout << "Frequency: " << center_freq << " Hz" << std::endl;
    std::cout << "Sample rate: " << sample_rate << " Hz" << std::endl;
    std::cout << "Gain: " << (gain >= 0 ? std::to_string(gain) : "auto") << std::endl;

    // Start SDR capture for SDRPage
    sdr.startCapture();
    std::cout << "SDR capture started" << std::endl;

    // Initialize audio output
    AudioOutput audio_output;
    if (!audio_output.init("default", 48000, 1)) {
        std::cerr << "Warning: audio output disabled" << std::endl;
    } else {
        std::cout << "Audio output initialized" << std::endl;
    }

    // Note: SDR capture will be started by SDRPage::onEnter()

    // First-run onboarding guide (keys + observer position).
    if (fb.isInitialized() && input.isReady() && guideShouldShow()) {
        runFirstRunGuide(fb, input);
    }

    // Create PageManager
    PageManager page_manager(fb, input);
    g_page_manager = &page_manager;

    // Create and register SDR page
    auto sdr_page = std::make_unique<SDRPage>(fb, sdr, audio_output, sdr_ui, input);
    page_manager.registerPage(std::move(sdr_page));

    // Create and register ADSB page (with radar view integrated)
    auto adsb_page = std::make_unique<ADSBPage>(fb, sdr, adsb_ui, radar_ui);
    page_manager.registerPage(std::move(adsb_page));

    std::cout << "Starting main loop with " << 2 << " pages..." << std::endl;

    // Run main loop (PageManager handles everything)
    int exit_code = page_manager.run();

    // Cleanup
    std::cout << "Shutting down..." << std::endl;
    input.restore();
    sdr.stopCapture();
    sdr.close();
    fb.clear(COLOR_BG_DARK);
    fb.update();

    return exit_code;
}
