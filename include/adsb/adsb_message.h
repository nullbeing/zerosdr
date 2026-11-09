#ifndef ADSB_MESSAGE_H
#define ADSB_MESSAGE_H

#include <cstdint>
#include <ctime>

/**
 * ADSB Message Types (Downlink Format)
 */
enum ADSBMessageType {
    DF_ADSB_EXTENDED_SQUITTER = 17,  // ADS-B Extended Squitter (most common)
    DF_TISB_EXTENDED_SQUITTER = 18,  // TIS-B Extended Squitter
    DF_MILITARY_EXTENDED_SQUITTER = 19  // Military Extended Squitter
};

/**
 * Type Code for DF17 messages (bits 33-37)
 */
enum ADSBTypeCode {
    TC_AIRCRAFT_IDENTIFICATION = 1,  // 1-4: Aircraft identification
    TC_SURFACE_POSITION = 5,         // 5-8: Surface position
    TC_AIRBORNE_POSITION = 9,        // 9-18: Airborne position (Baro Alt)
    TC_AIRBORNE_VELOCITY = 19,       // 19: Airborne velocity
    TC_AIRBORNE_POSITION_GNSS = 20,  // 20-22: Airborne position (GNSS Height)
};

/**
 * Aircraft state information
 */
struct AircraftState {
    uint32_t icao_addr;          // 24-bit ICAO address
    char callsign[9];            // 8-char callsign + null terminator

    // Position
    double latitude;             // Degrees
    double longitude;            // Degrees
    int32_t altitude;            // Feet (barometric)
    bool position_valid;         // Position data available

    // Velocity
    int16_t speed;               // Knots (ground speed)
    int16_t heading;             // Degrees (0-359)
    int16_t vertical_rate;       // Feet per minute
    bool velocity_valid;         // Velocity data available

    // Metadata
    time_t last_seen;            // Timestamp of last message
    uint32_t message_count;      // Total messages received

    // Derived data
    float distance;              // Distance from observer (km)
    float bearing;               // Bearing from observer (degrees)

    AircraftState() : icao_addr(0), latitude(0), longitude(0), altitude(0),
                      position_valid(false), speed(0), heading(0), vertical_rate(0),
                      velocity_valid(false), last_seen(0), message_count(0),
                      distance(0), bearing(0) {
        callsign[0] = '\0';
    }
};

/**
 * Raw Mode S message (56 or 112 bits)
 */
struct ModeSMessage {
    uint8_t data[14];            // Raw message bytes (max 112 bits = 14 bytes)
    uint8_t length;              // Message length in bytes (7 or 14)
    uint32_t timestamp;          // Timestamp (microseconds)

    // Decoded fields
    uint8_t downlink_format;     // DF (bits 1-5)
    uint32_t icao_addr;          // 24-bit ICAO address
    uint8_t type_code;           // Type code for DF17 (bits 33-37)

    ModeSMessage() : length(0), timestamp(0), downlink_format(0),
                     icao_addr(0), type_code(0) {
        for (int i = 0; i < 14; i++) data[i] = 0;
    }
};

#endif // ADSB_MESSAGE_H
