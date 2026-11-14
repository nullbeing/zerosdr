#include "system_volume.h"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace {

constexpr int kConnectTimeoutMs = 800;
constexpr int kOperationTimeoutMs = 500;

struct SinkQuery {
    bool done = false;
    bool ok = false;
    int percent = 0;
    uint8_t channels = 0;
    uint32_t index = 0xFFFFFFFFu;
};

struct OperationResult {
    bool done = false;
    bool ok = false;
};

void successCallback(pa_context*, int success, void* userdata) {
    auto* result = static_cast<OperationResult*>(userdata);
    result->ok = success != 0;
    result->done = true;
}

void sinkInfoCallback(pa_context*, const pa_sink_info* info, int eol,
                      void* userdata) {
    auto* query = static_cast<SinkQuery*>(userdata);
    if (eol != 0 || info == nullptr || info->volume.channels == 0) {
        query->done = true;
        return;
    }

    uint64_t sum = 0;
    for (uint8_t i = 0; i < info->volume.channels; ++i) {
        sum += info->volume.values[i];
    }
    const double average =
        static_cast<double>(sum) / static_cast<double>(info->volume.channels);
    query->percent = static_cast<int>(std::lround(
        100.0 * average / static_cast<double>(PA_VOLUME_NORM)));
    query->channels = info->volume.channels;
    query->index = info->index;
    query->ok = true;
    query->done = true;
}

pa_mainloop* loopHandle(void* handle) {
    return static_cast<pa_mainloop*>(handle);
}

pa_context* contextHandle(void* handle) {
    return static_cast<pa_context*>(handle);
}

}  // namespace

SystemVolume::SystemVolume() = default;

SystemVolume::~SystemVolume() {
    if (context_ != nullptr) {
        pa_context_disconnect(contextHandle(context_));
        pa_context_unref(contextHandle(context_));
        context_ = nullptr;
    }
    if (mainloop_ != nullptr) {
        pa_mainloop_free(loopHandle(mainloop_));
        mainloop_ = nullptr;
    }
}

bool SystemVolume::init() {
    if (ready_) return true;

    mainloop_ = pa_mainloop_new();
    if (mainloop_ == nullptr) {
        error_ = "cannot create PulseAudio main loop";
        return false;
    }

    context_ = pa_context_new(pa_mainloop_get_api(loopHandle(mainloop_)),
                              "zeroSDR");
    if (context_ == nullptr) {
        error_ = "cannot create PulseAudio context";
        return false;
    }

    if (pa_context_connect(contextHandle(context_), nullptr,
                           PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        error_ = "cannot connect to the sound server";
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kConnectTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const pa_context_state_t state = pa_context_get_state(
            contextHandle(context_));
        if (state == PA_CONTEXT_READY) {
            ready_ = true;
            error_ = "";
            return true;
        }
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            error_ = "sound server connection failed";
            return false;
        }
        int retval = 0;
        if (pa_mainloop_iterate(loopHandle(mainloop_), 0, &retval) < 0) {
            error_ = "sound server main loop failed";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    error_ = "sound server connection timed out";
    return false;
}

bool SystemVolume::read(int& percent) {
    if (!ready_) return false;

    SinkQuery query;
    pa_operation* operation = pa_context_get_sink_info_by_name(
        contextHandle(context_), nullptr, sinkInfoCallback, &query);
    if (operation == nullptr) return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kOperationTimeoutMs);
    while (!query.done && std::chrono::steady_clock::now() < deadline) {
        int retval = 0;
        if (pa_mainloop_iterate(loopHandle(mainloop_), 0, &retval) < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pa_operation_unref(operation);

    if (!query.ok) return false;
    if (query.channels > 0) channels_ = query.channels;
    sink_index_ = query.index;
    percent = std::clamp(query.percent, 0, 100);
    return true;
}

bool SystemVolume::write(int percent) {
    if (!ready_) return false;
    percent = std::clamp(percent, 0, 100);

    // Refresh the sink index first; it changes when the server restarts.
    int ignored = 0;
    if (!read(ignored)) return false;

    pa_cvolume volume;
    pa_cvolume_init(&volume);
    const pa_volume_t value = static_cast<pa_volume_t>(
        std::lround(static_cast<double>(percent) / 100.0 *
                    static_cast<double>(PA_VOLUME_NORM)));
    pa_cvolume_set(&volume, channels_, value);

    OperationResult result;
    pa_operation* operation = pa_context_set_sink_volume_by_index(
        contextHandle(context_), sink_index_, &volume, successCallback,
        &result);
    if (operation == nullptr) return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kOperationTimeoutMs);
    while (!result.done && std::chrono::steady_clock::now() < deadline) {
        int retval = 0;
        if (pa_mainloop_iterate(loopHandle(mainloop_), 0, &retval) < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pa_operation_unref(operation);
    if (!result.ok) return false;

    // Volume-up should also lift a mute left over from the launcher.
    if (percent > 0) {
        OperationResult unmuted;
        pa_operation* mute = pa_context_set_sink_mute_by_index(
            contextHandle(context_), sink_index_, 0, successCallback,
            &unmuted);
        if (mute != nullptr) {
            const auto mute_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(
                                           kOperationTimeoutMs);
            while (!unmuted.done &&
                   std::chrono::steady_clock::now() < mute_deadline) {
                int retval = 0;
                if (pa_mainloop_iterate(loopHandle(mainloop_), 0, &retval) < 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            pa_operation_unref(mute);
        }
    }
    return true;
}
