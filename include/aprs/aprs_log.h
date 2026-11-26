#ifndef APRS_LOG_H
#define APRS_LOG_H

#include "aprs/aprs_packet.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace aprs {

/**
 * Daily JSONL history for APRS packets.
 *
 * Each decoded packet is appended to ~/.local/share/zerosdr/aprs/
 * YYYY-MM-DD.jsonl. On startup the last seven daily files are replayed into
 * the store, so stations, tracks, weather and the console survive restarts.
 * Files older than seven days are deleted.
 */
class Log {
public:
    bool append(const Packet& packet, uint32_t frequency, time_t when);
    std::vector<std::pair<time_t, Packet>> loadRecent(uint32_t frequency,
                                                      int days = 7) const;
    void pruneOlderThan(int days = 7) const;

    static std::string directory();

private:
    static std::string dateString(time_t when);
    static std::string cutoffDate(int days);
};

}  // namespace aprs

#endif  // APRS_LOG_H
