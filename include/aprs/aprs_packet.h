#ifndef APRS_PACKET_H
#define APRS_PACKET_H

#include <string>
#include <vector>

namespace aprs {

/**
 * One decoded APRS packet. Only the fields needed by the page are kept; the
 * raw AX.25 information field is preserved for the console.
 */
struct Packet {
    std::string source;
    std::string destination;
    std::vector<std::string> path;
    std::string info;

    char type = 0;  // first character of the information field

    bool has_position = false;
    double latitude = 0.0;
    double longitude = 0.0;
    float speed_knots = -1.0f;
    float course = -1.0f;
    int altitude_ft = -100000;
    char symbol_table = '/';
    char symbol = '>';

    std::string comment;

    bool weather = false;
    float wind_dir = -1.0f;         // degrees
    float wind_speed_mph = -1.0f;   // APRS weather uses mph
    float wind_gust_mph = -1.0f;
    float temperature_c = 0.0f;
    bool has_temperature = false;
    float humidity = -1.0f;         // percent
    float pressure_hpa = -1.0f;
    float rain_hour_mm = -1.0f;
    float rain_24h_mm = -1.0f;
    float rain_midnight_mm = -1.0f;

    bool message = false;
    std::string addressee;
    std::string message_text;
};

}  // namespace aprs

#endif  // APRS_PACKET_H
