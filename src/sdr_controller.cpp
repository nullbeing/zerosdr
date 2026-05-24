#include "sdr_controller.h"
#include <iostream>
#include <cstring>
#include <algorithm>

SDRController::SDRController()
    : device(nullptr), initialized(false), agc_enabled(false), capturing(false),
      current_frequency(100000000), current_sample_rate(2048000),
      current_gain(0), ring_buffer_size(1048576), write_pos(0), read_pos(0) {
    // Ring buffer: 1MB = ~0.5 seconds at 2.048 MHz
    ring_buffer.resize(ring_buffer_size);
}

SDRController::~SDRController() {
    close();
}

bool SDRController::init(uint32_t device_index) {
    int device_count = rtlsdr_get_device_count();
    if (device_count == 0) {
        std::cerr << "No RTL-SDR devices found" << std::endl;
        return false;
    }

    if (device_index >= (uint32_t)device_count) {
        std::cerr << "Invalid device index: " << device_index << std::endl;
        return false;
    }

    int ret = rtlsdr_open(&device, device_index);
    if (ret < 0) {
        std::cerr << "Failed to open RTL-SDR device: " << ret << std::endl;
        return false;
    }

    // Reset device
    rtlsdr_reset_buffer(device);

    initialized = true;
    return true;
}

void SDRController::close() {
    if (capturing) {
        stopCapture();
    }

    if (device != nullptr) {
        rtlsdr_close(device);
        device = nullptr;
    }

    initialized = false;
}

bool SDRController::setFrequency(uint32_t freq_hz) {
    if (!initialized) return false;

    int ret = rtlsdr_set_center_freq(device, freq_hz);
    if (ret < 0) {
        std::cerr << "Failed to set frequency: " << ret << std::endl;
        return false;
    }

    current_frequency = rtlsdr_get_center_freq(device);
    return true;
}

bool SDRController::setSampleRate(uint32_t rate) {
    if (!initialized) return false;

    int ret = rtlsdr_set_sample_rate(device, rate);
    if (ret < 0) {
        std::cerr << "Failed to set sample rate: " << ret << std::endl;
        return false;
    }

    current_sample_rate = rtlsdr_get_sample_rate(device);
    return true;
}

bool SDRController::setGain(int gain) {
    if (!initialized) return false;

    int ret = rtlsdr_set_tuner_gain(device, gain * 10);
    if (ret < 0) {
        std::cerr << "Failed to set gain: " << ret << std::endl;
        return false;
    }

    current_gain = rtlsdr_get_tuner_gain(device) / 10;
    return true;
}

bool SDRController::setAutoGain(bool enable) {
    if (!initialized) return false;

    int ret = rtlsdr_set_tuner_gain_mode(device, enable ? 0 : 1);
    if (ret < 0) {
        std::cerr << "Failed to set auto gain mode: " << ret << std::endl;
        return false;
    }

    return true;
}

bool SDRController::setAGCMode(bool enable) {
    if (!initialized) return false;

    // Set digital AGC (RTL2832 chip)
    int ret = rtlsdr_set_agc_mode(device, enable ? 1 : 0);
    if (ret < 0) {
        std::cerr << "Failed to set AGC mode: " << ret << std::endl;
        return false;
    }

    // Set tuner auto gain mode (R820T2 tuner chip)
    ret = rtlsdr_set_tuner_gain_mode(device, enable ? 0 : 1);
    if (ret < 0) {
        std::cerr << "Failed to set tuner gain mode: " << ret << std::endl;
        return false;
    }

    agc_enabled = enable;
    return true;
}

bool SDRController::startCapture() {
    if (!initialized || capturing) return false;

    rtlsdr_reset_buffer(device);

    // Reset ring buffer positions
    write_pos = 0;
    read_pos = 0;

    capturing = true;
    capture_thread = std::thread(&SDRController::captureLoop, this);

    return true;
}

void SDRController::stopCapture() {
    if (!capturing) return;

    capturing = false;
    rtlsdr_cancel_async(device);

    if (capture_thread.joinable()) {
        capture_thread.join();
    }
}

bool SDRController::getLatestSamples(std::vector<uint8_t>& samples, size_t count) {
    if (!capturing) return false;

    size_t available = getAvailableSamples();
    if (available < count) {
        // Not enough data yet
        return false;
    }

    samples.resize(count);
    size_t current_read = read_pos.load();

    // Read from ring buffer (handle wrap-around)
    for (size_t i = 0; i < count; i++) {
        samples[i] = ring_buffer[(current_read + i) % ring_buffer_size];
    }

    // Update read position
    read_pos = (current_read + count) % ring_buffer_size;

    return true;
}

size_t SDRController::getAvailableSamples() const {
    size_t w = write_pos.load();
    size_t r = read_pos.load();

    if (w >= r) {
        return w - r;
    } else {
        return ring_buffer_size - r + w;
    }
}

void SDRController::rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    SDRController* controller = static_cast<SDRController*>(ctx);

    if (len == 0 || !controller->capturing) return;

    size_t current_write = controller->write_pos.load();

    // Write to ring buffer (handle wrap-around)
    for (uint32_t i = 0; i < len; i++) {
        controller->ring_buffer[(current_write + i) % controller->ring_buffer_size] = buf[i];
    }

    // Update write position
    controller->write_pos = (current_write + len) % controller->ring_buffer_size;

    // Removed buffer warning - this is normal during continuous operation
}

void SDRController::captureLoop() {
    // Use smaller buffer size for RTL-SDR callback (16KB)
    rtlsdr_read_async(device, rtlsdr_callback, this, 0, 16384);
}
