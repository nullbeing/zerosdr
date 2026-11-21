#ifndef AIS_NMEA_ENCODER_H
#define AIS_NMEA_ENCODER_H

#include "ais/ais_message.h"

#include <cstdint>
#include <deque>
#include <netinet/in.h>
#include <string>
#include <utility>
#include <vector>

namespace ais {

/**
 * NmeaEncoder - turn a validated AIS frame into !AIVDM sentences.
 *
 * AIS frames arrive least-significant bit first within each byte. The NMEA
 * payload uses the normal MSB-first field order, so every byte is reversed
 * before the 6-bit armour packing.
 */
class NmeaEncoder {
public:
    std::vector<std::string> encode(const AisFrame& frame) const;

    /**
     * Cross-channel duplicate suppression for a 1 second window. The same
     * message is often received on AIS1 and AIS2; a NMEA feed should not
     * forward it twice.
     */
    bool isDuplicate(const AisFrame& frame);

private:
    std::deque<std::pair<uint64_t, double>> recent_;
};

/** Small IPv4 UDP sender used for the standard NMEA port (10110). */
class NmeaUdpSender {
public:
    ~NmeaUdpSender();

    bool open(const std::string& host, int port);
    void close();
    bool send(const std::string& sentence);
    bool isOpen() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    struct sockaddr_in address_ {};
};

}  // namespace ais

#endif  // AIS_NMEA_ENCODER_H
