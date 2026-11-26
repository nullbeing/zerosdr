#ifndef APRS_DECODER_H
#define APRS_DECODER_H

#include "aprs/aprs_packet.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace aprs {

/**
 * Native 1200 baud APRS receiver.
 *
 * IQ samples (1.2 Msps, unsigned 8-bit interleaved) are FM-demodulated,
 * decimated to 48 kHz and fed to a Bell 202 AFSK detector. Ten bit-clock
 * phases cover one 40-sample bit; each phase runs its own AX.25/HDLC
 * deframer and CRC-16/X.25 check, so no external TNC or sound card is
 * involved.
 */
class Decoder {
public:
    struct Config {
        double sample_rate = 1200000.0;
        int audio_rate = 48000;
    };

    struct Stats {
        uint64_t samples = 0;
        uint64_t packets = 0;
        uint64_t crc_fail = 0;
        uint64_t duplicates = 0;
        uint64_t bits = 0;
        uint64_t transitions = 0;
    };

    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool init(const Config& config);
    void reset();
    void process(const uint8_t* cu8, size_t sample_count);
    // Feed already demodulated audio at 48 kHz. Used by the offline audio
    // replay tool and by tests; the normal path goes through process().
    void processAudio(const float* samples, size_t sample_count);
    void setPacketCallback(std::function<void(const Packet&)> callback);
    Stats stats() const;
    const Config& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aprs

#endif  // APRS_DECODER_H
