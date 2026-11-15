#ifndef SYSTEM_VOLUME_H
#define SYSTEM_VOLUME_H

#include <cstdint>

// Read the default PulseAudio/PipeWire sink volume.
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

// Process-wide volume helper shared by all pages. The main loop applies the
// fn+D / fn+S keys through stepSystemVolume() so they work whatever page is
// in the foreground; pages read systemVolumePercent() for display.
bool systemVolumeInit();
bool systemVolumeAvailable();
const char* systemVolumeError();

// Cached value, refreshed by the calls below. -1 when unavailable.
int systemVolumePercent();

// Re-read the sink volume. Returns the percentage, or -1 on failure.
int systemVolumeRefresh();

// Change the sink volume by delta percent (clamped to 0-100) and return the
// new percentage, or -1 when the sound server is unavailable.
int systemVolumeStep(int delta_percent);

#endif  // SYSTEM_VOLUME_H
