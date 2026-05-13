#include "sdr_controller.h"
#include <iostream>
#include <cstring>

SDRController::SDRController()
    : device(nullptr), initialized(false), capturing(false),
      current_frequency(100000000), current_sample_rate(2048000),
      current_gain(0), data_ready(false), buffer_size(262144) {
    sample_buffer.resize(buffer_size);
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

    int ret = rtlsdr_set_tuner_gain(device, gain * 10); // Gain is in tenths of dB
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

bool SDRController::startCapture() {
    if (!initialized || capturing) return false;

    rtlsdr_reset_buffer(device);
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
    if (!capturing || !data_ready) return false;

    std::lock_guard<std::mutex> lock(buffer_mutex);

    size_t copy_count = std::min(count, sample_buffer.size());
    samples.resize(copy_count);
    std::memcpy(samples.data(), sample_buffer.data(), copy_count);

    return true;
}

void SDRController::rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    SDRController* controller = static_cast<SDRController*>(ctx);

    if (len > 0 && len <= controller->buffer_size) {
        std::lock_guard<std::mutex> lock(controller->buffer_mutex);
        std::memcpy(controller->sample_buffer.data(), buf, len);
        controller->data_ready = true;
    }
}

void SDRController::captureLoop() {
    rtlsdr_read_async(device, rtlsdr_callback, this, 0, buffer_size);
}
