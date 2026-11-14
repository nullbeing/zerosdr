#ifndef SYSTEM_VOLUME_H
#define SYSTEM_VOLUME_H

#include <cstdint>

// Read the default PulseAudio/PipeWire sink volume. The launcher already
// handles the Cardputer Zero volume keys (fn+D / fn+S) through labwc, so the
// viewer only follows the system volume instead of implementing its own.
class SystemVolume {
public:
    SystemVolume();
    ~SystemVolume();

    SystemVolume(const SystemVolume&) = delete;
    SystemVolume& operator=(const SystemVolume&) = delete;

    bool init();

    // 0-100, matching the percentage shown by APPLaunch and wpctl.
    bool read(int& percent);
    bool write(int percent);

    const char* error() const { return error_; }

private:
    void* mainloop_ = nullptr;
    void* context_ = nullptr;
    uint8_t channels_ = 2;
    uint32_t sink_index_ = 0xFFFFFFFFu;
    bool ready_ = false;
    const char* error_ = "";
};

#endif  // SYSTEM_VOLUME_H
