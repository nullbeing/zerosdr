#ifndef AIS_DECODER_H
#define AIS_DECODER_H

#include "ais/ais_message.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace ais {

/**
 * AisDecoder - native dual-channel AIS receiver.
 *
 * Input is interleaved 8-bit unsigned IQ (the RTL-SDR CU8 format), centred
 * between AIS1 (161.975 MHz) and AIS2 (162.025 MHz). The decoder mixes each
 * channel to baseband, filters and decimates to 48 kHz, FM-discriminates,
 * and runs five fixed sampling phases per channel through an HDLC framer.
 * The five phases cover one 9600-baud bit period, so no adaptive timing loop
 * is needed for the short AIS bursts.
 *
 * Valid frames (CRC-16/X-25 checked) are delivered through the callback.
 */
class AisDecoder {
public:
    struct Config {
        double sample_rate = 1920000.0;
        double centre_frequency = 162000000.0;
        double ppm = 0.0;
        double channel_offset_hz = 25000.0;
        bool channel_a = true;
        bool channel_b = true;
        double stage2_cutoff_hz = 7000.0;
        double discriminator_cutoff_hz = 6800.0;
    };

    struct Stats {
        uint64_t samples = 0;
        uint64_t frames = 0;
        uint64_t duplicates = 0;
        uint64_t flags = 0;
        uint64_t crc_fail = 0;
        uint64_t aborts = 0;
    };

    using FrameCallback = std::function<void(const AisFrame&)>;

    AisDecoder();
    ~AisDecoder();

    AisDecoder(const AisDecoder&) = delete;
    AisDecoder& operator=(const AisDecoder&) = delete;

    bool init(const Config& config);
    void reset();

    /** Feed interleaved CU8 samples. sample_count is the number of IQ pairs. */
    void process(const uint8_t* cu8, size_t sample_count);

    void setFrameCallback(FrameCallback callback);
    Stats stats() const;
    const Config& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ais

#endif  // AIS_DECODER_H
