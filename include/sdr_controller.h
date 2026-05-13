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

    bool startCapture();
    void stopCapture();
    bool isCapturing() const { return capturing; }

    // Get latest IQ samples
    bool getLatestSamples(std::vector<uint8_t>& samples, size_t count);

private:
    rtlsdr_dev_t* device;
    bool initialized;
    std::atomic<bool> capturing;

    uint32_t current_frequency;
    uint32_t current_sample_rate;
    int current_gain;

    std::thread capture_thread;
    std::vector<uint8_t> sample_buffer;
    std::mutex buffer_mutex;
    std::atomic<bool> data_ready;
    size_t buffer_size;

    static void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx);
    void captureLoop();
};

#endif // SDR_CONTROLLER_H
