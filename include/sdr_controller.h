#ifndef SDR_CONTROLLER_H
#define SDR_CONTROLLER_H

#include <rtl-sdr.h>
#include <stdint.h>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

class SDRController {
public:
    SDRController();
    ~SDRController();

    bool init(uint32_t device_index = 0);
    void close();

    bool setFrequency(uint32_t freq_hz);
    bool setSampleRate(uint32_t rate);
    bool setGain(int gain);
    bool setAutoGain(bool enable);

    uint32_t getFrequency() const { return current_frequency; }
    uint32_t getSampleRate() const { return current_sample_rate; }
    int getGain() const { return current_gain; }

    bool setAGCMode(bool enable);
    bool getAGCEnabled() const { return agc_enabled; }

    bool startCapture();
    void stopCapture();
    bool isCapturing() const { return capturing; }

    // Get continuous IQ samples from ring buffer
    bool getLatestSamples(std::vector<uint8_t>& samples, size_t count);

private:
    rtlsdr_dev_t* device;
    bool initialized;
    bool agc_enabled;
    std::atomic<bool> capturing;

    uint32_t current_frequency;
    uint32_t current_sample_rate;
    int current_gain;

    std::thread capture_thread;

    // Ring buffer for continuous sample storage
    std::vector<uint8_t> ring_buffer;
    size_t ring_buffer_size;
    std::atomic<size_t> write_pos;  // Where RTL-SDR writes
    std::atomic<size_t> read_pos;   // Where we read from
    std::mutex buffer_mutex;

    static void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx);
    void captureLoop();

    // Helper: get available samples in ring buffer
    size_t getAvailableSamples() const;
};

#endif // SDR_CONTROLLER_H
