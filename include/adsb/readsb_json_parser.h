#ifndef READSB_JSON_PARSER_H
#define READSB_JSON_PARSER_H

#include "adsb/adsb_message.h"
#include <map>
#include <string>

/**
 * READSBJSONParser - Parses aircraft.json from readsb
 *
 * Reads and parses the JSON output from readsb to populate
 * the aircraft database with real-time ADSB data.
 */
class READSBJSONParser {
public:
    READSBJSONParser();

    /**
     * Parse aircraft.json and update aircraft database
     * @param json_path Path to aircraft.json file
     * @param aircraft_db Aircraft database to update
     * @return true if parsing succeeded
     */
    bool parseFile(const std::string& json_path,
                   std::map<uint32_t, AircraftState>& aircraft_db);

private:
    /**
     * Parse hex ICAO address string to uint32_t
     * @param hex_str Hex string (e.g., "a12345")
     * @return ICAO address as uint32_t
     */
    uint32_t parseHexICAO(const char* hex_str);

    /**
     * Find JSON field value in string
     * @param json JSON string
     * @param field Field name
     * @return Pointer to field value or nullptr
     */
    const char* findField(const char* json, const char* field);

    /**
     * Extract string value from JSON field
     * @param field_value Pointer to field value
     * @param buffer Output buffer
     * @param buffer_size Buffer size
     */
    void extractString(const char* field_value, char* buffer, size_t buffer_size);

    /**
     * Extract integer value from JSON field
     * @param field_value Pointer to field value
     * @return Integer value
     */
    int extractInt(const char* field_value);

    /**
     * Extract float value from JSON field
     * @param field_value Pointer to field value
     * @return Float value
     */
    float extractFloat(const char* field_value);
};

#endif // READSB_JSON_PARSER_H
