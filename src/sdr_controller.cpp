#include "sdr_controller.h"
#include <iostream>
#include <cstring>
#include <algorithm>

SDRController::SDRController()
    : device(nullptr), initialized(false), agc_enabled(false), capturing(false),
      current_frequency(100000000), current_sample_rate(2048000),
      current_gain(0), ring_buffer_size(2097152), write_pos(0), read_pos(0) {
    // Ring buffer: 2MB = ~2 seconds at 1.024 MHz, ~0.87 s at 2.4 MHz.
    // Extra headroom absorbs scheduling jitter without dropping USB data.
    ring_buffer.resize(ring_buffer_size);
}

SDRController::~SDRController() {
    close();
}

bool SDRController::init(uint32_t device_index) {
    // Re-initialising after another page left the device open would leak the
    // old handle and fail with EBUSY.
    if (device != nullptr) close();

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

    // The RTL2832 digital AGC makes abrupt gain changes that can sound like
    // brief audio dropouts. Keep it off; use the tuner's auto/manual gain.
    int ret = rtlsdr_set_agc_mode(device, 0);
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

void SDRController::flushBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    read_pos = write_pos.load();
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

    std::lock_guard<std::mutex> lock(buffer_mutex);

    size_t available = getAvailableSamples();
    if (available < count) {
        // Not enough data yet
        return false;
    }

    samples.resize(count);
    size_t current_read = read_pos.load();

    // Two-segment memcpy to handle ring buffer wrap-around
    size_t first_chunk = std::min(count, ring_buffer_size - current_read);
    memcpy(samples.data(), &ring_buffer[current_read], first_chunk);
    if (count > first_chunk) {
        memcpy(samples.data() + first_chunk, &ring_buffer[0], count - first_chunk);
    }

    // Update read position
    read_pos = (current_read + count) % ring_buffer_size;

    return true;
}

bool SDRController::getLatestSamplesSnapshot(std::vector<uint8_t>& samples, size_t count) {
    if (!capturing || count == 0 || count > ring_buffer_size) return false;

    std::lock_guard<std::mutex> lock(buffer_mutex);

    // The newest `count` bytes end at write_pos. This does not advance
    // read_pos, so the audio thread can keep consuming the same stream.
    size_t current_write = write_pos.load();
    size_t start = (current_write + ring_buffer_size - count) % ring_buffer_size;

    samples.resize(count);
    size_t first_chunk = std::min(count, ring_buffer_size - start);
    memcpy(samples.data(), &ring_buffer[start], first_chunk);
    if (count > first_chunk) {
        memcpy(samples.data() + first_chunk, &ring_buffer[0], count - first_chunk);
    }

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

    std::lock_guard<std::mutex> lock(controller->buffer_mutex);

    size_t current_write = controller->write_pos.load();

    // Detect overflow: if write would overtake read, drop new data
    size_t free_space = (controller->read_pos.load() + controller->ring_buffer_size - current_write - 1)
                        % controller->ring_buffer_size;
    if (len > free_space) {
        // Drop incoming data to avoid corruption
        controller->dropped_bytes += len;
        return;
    }

    // Two-segment memcpy to handle ring buffer wrap-around
    size_t first_chunk = std::min((size_t)len, controller->ring_buffer_size - current_write);
    memcpy(&controller->ring_buffer[current_write], buf, first_chunk);
    if (len > first_chunk) {
        memcpy(&controller->ring_buffer[0], buf + first_chunk, len - first_chunk);
    }

    // Update write position
    controller->write_pos = (current_write + len) % controller->ring_buffer_size;
}

void SDRController::captureLoop() {
    // 32 x 32 KB = 1 MB of USB transfer buffering. The default (15 x 16 KB)
    // only gives ~120 ms at 1.024 MHz; a short scheduler/USB stall can then
    // drop URBs and produce an audible click every few seconds.
    rtlsdr_read_async(device, rtlsdr_callback, this, 32, 32768);
}
