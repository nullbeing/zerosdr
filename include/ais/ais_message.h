#ifndef AIS_MESSAGE_H
#define AIS_MESSAGE_H

#include <array>
#include <cstdint>
#include <string>

namespace ais {

// One validated HDLC frame: the destuffed AIS bits, including the 16-bit CRC.
struct AisFrame {
    std::array<uint8_t, 140> bits{};  // MSB-first, up to 1120 bits
    int bit_length = 0;               // includes the 16-bit CRC
    uint8_t channel = 0;              // 0 = AIS1 (161.975), 1 = AIS2 (162.025)
    uint8_t phase = 0;                // sampling phase (0..4)
    float signal_db = 0.0f;
    double timestamp = 0.0;           // seconds since the decoder started
};

// Decoded fields. Only the fields relevant to the message type are filled.
struct AisMessage {
    int type = 0;
    uint32_t mmsi = 0;
    int repeat = 0;

    // Dynamic data
    bool has_position = false;
    double latitude = 0.0;
    double longitude = 0.0;
    float sog = -1.0f;                // knots, -1 = not available
    float cog = -1.0f;                // degrees true, -1 = not available
    int heading = -1;                 // degrees, -1 = not available
    int nav_status = -1;
    float rot = 0.0f;                 // degrees / minute
    bool rot_valid = false;
    int utc_second = -1;

    // Static / voyage data
    std::string name;
    std::string callsign;
    std::string destination;
    int ship_type = -1;
    uint32_t imo = 0;
    int to_bow = -1;
    int to_stern = -1;
    int to_port = -1;
    int to_starboard = -1;
    float draught = -1.0f;
    int eta_month = 0;
    int eta_day = 0;
    int eta_hour = 0;
    int eta_minute = 0;
    int epfd = -1;
    bool class_b = false;
    bool is_aton = false;
    bool is_base_station = false;
    bool off_position = false;
    bool virtual_aton = false;

    // UTC date/time from type 4
    int utc_year = 0;
    int utc_month = 0;
    int utc_day = 0;
    int utc_hour = 0;
    int utc_minute = 0;

    bool valid = false;

    void clear();
};

// Parse a validated frame. Returns false when the bit layout is unusable.
bool parseAisMessage(const AisFrame& frame, AisMessage& out);

// 6-bit ASCII helpers (used by the message parser and later by NMEA output).
char sixBitToChar(uint8_t value);
std::string sixBitString(const uint8_t* data, int bit_offset, int bit_length);

}  // namespace ais

#endif  // AIS_MESSAGE_H
