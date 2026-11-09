#include "adsb/readsb_json_parser.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <ctime>

READSBJSONParser::READSBJSONParser() {
}

uint32_t READSBJSONParser::parseHexICAO(const char* hex_str) {
    return (uint32_t)strtoul(hex_str, nullptr, 16);
}

const char* READSBJSONParser::findField(const char* json, const char* field) {
    // Simple JSON field finder: looks for "field":
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", field);
    return strstr(json, search);
}

void READSBJSONParser::extractString(const char* field_value, char* buffer, size_t buffer_size) {
    // Skip to colon first
    const char* colon = strchr(field_value, ':');
    if (!colon) {
        buffer[0] = '\0';
        return;
    }

    // Find opening quote after colon
    const char* start = strchr(colon, '"');
    if (!start) {
        buffer[0] = '\0';
        return;
    }
    start++;  // Skip opening quote

    // Find closing quote
    const char* end = strchr(start, '"');
    if (!end) {
        buffer[0] = '\0';
        return;
    }

    // Copy string
    size_t len = end - start;
    if (len >= buffer_size) {
        len = buffer_size - 1;
    }
    memcpy(buffer, start, len);
    buffer[len] = '\0';

    // Trim trailing whitespace
    while (len > 0 && buffer[len - 1] == ' ') {
        buffer[--len] = '\0';
    }
}

int READSBJSONParser::extractInt(const char* field_value) {
    // Skip to colon
    const char* colon = strchr(field_value, ':');
    if (!colon) return 0;

    // Skip whitespace after colon
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;

    return atoi(colon);
}

float READSBJSONParser::extractFloat(const char* field_value) {
    // Skip to colon
    const char* colon = strchr(field_value, ':');
    if (!colon) return 0.0f;

    // Skip whitespace after colon
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;

    return (float)atof(colon);
}

bool READSBJSONParser::parseFile(const std::string& json_path,
                                  std::map<uint32_t, AircraftState>& aircraft_db) {
    // Read entire file into memory
    std::ifstream file(json_path);
    if (!file.is_open()) {
        return false;
    }

    std::string json_content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    file.close();

    if (json_content.empty()) {
        return false;
    }

    const char* json = json_content.c_str();

    // Find "aircraft" array
    const char* aircraft_array = strstr(json, "\"aircraft\"");
    if (!aircraft_array) {
        return false;
    }

    // Find opening bracket of array
    const char* array_start = strchr(aircraft_array, '[');
    if (!array_start) {
        return false;
    }

    // Get current time for calculating last_seen
    time_t now = time(nullptr);

    // Clear existing database
    aircraft_db.clear();

    // Parse each aircraft object in array
    const char* pos = array_start + 1;
    while (*pos != '\0') {
        // Skip whitespace
        while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;

        // Check for array end
        if (*pos == ']') break;

        // Find next object start
        const char* obj_start = strchr(pos, '{');
        if (!obj_start) break;

        // Find object end with proper bracket matching
        const char* obj_end = obj_start + 1;
        int brace_depth = 1;
        bool in_string = false;
        bool escape_next = false;

        while (*obj_end != '\0' && brace_depth > 0) {
            if (escape_next) {
                escape_next = false;
                obj_end++;
                continue;
            }

            if (*obj_end == '\\') {
                escape_next = true;
            } else if (*obj_end == '"') {
                in_string = !in_string;
            } else if (!in_string) {
                if (*obj_end == '{') {
                    brace_depth++;
                } else if (*obj_end == '}') {
                    brace_depth--;
                }
            }
            obj_end++;
        }

        if (brace_depth != 0) break;  // Malformed JSON
        obj_end--;  // Point to the closing brace

        // Extract object substring
        size_t obj_len = obj_end - obj_start + 1;
        char obj_str[4096];
        if (obj_len >= sizeof(obj_str)) {
            pos = obj_end + 1;
            continue;
        }
        memcpy(obj_str, obj_start, obj_len);
        obj_str[obj_len] = '\0';

        // Parse aircraft fields
        AircraftState ac;
        memset(&ac, 0, sizeof(ac));

        // Parse ICAO address (required)
        const char* hex_field = findField(obj_str, "hex");
        if (hex_field) {
            char hex_str[16];
            extractString(hex_field, hex_str, sizeof(hex_str));
            ac.icao_addr = parseHexICAO(hex_str);
        } else {
            // Skip aircraft without ICAO
            pos = obj_end + 1;
            continue;
        }

        // Parse callsign (optional)
        const char* flight_field = findField(obj_str, "flight");
        if (flight_field) {
            extractString(flight_field, ac.callsign, sizeof(ac.callsign));
        }

        // Parse altitude (optional)
        const char* alt_field = findField(obj_str, "alt_baro");
        if (alt_field) {
            ac.altitude = extractInt(alt_field);
        }

        // Parse position (optional)
        const char* lat_field = findField(obj_str, "lat");
        const char* lon_field = findField(obj_str, "lon");
        if (lat_field && lon_field) {
            ac.latitude = extractFloat(lat_field);
            ac.longitude = extractFloat(lon_field);
            ac.position_valid = true;
        }

        // Parse velocity (optional)
        const char* gs_field = findField(obj_str, "gs");
        const char* track_field = findField(obj_str, "track");
        if (gs_field && track_field) {
            ac.speed = (int)extractFloat(gs_field);
            ac.heading = (int)extractFloat(track_field);
            ac.velocity_valid = true;
        }

        // Parse vertical rate (optional)
        const char* baro_rate_field = findField(obj_str, "baro_rate");
        if (baro_rate_field) {
            ac.vertical_rate = extractInt(baro_rate_field);
        }

        // Parse seen time (optional)
        const char* seen_field = findField(obj_str, "seen");
        if (seen_field) {
            float seen = extractFloat(seen_field);
            ac.last_seen = now - (time_t)seen;
        } else {
            ac.last_seen = now;
        }

        // Parse message count (optional)
        const char* messages_field = findField(obj_str, "messages");
        if (messages_field) {
            ac.message_count = (uint32_t)extractInt(messages_field);
        }

        // Add to database
        aircraft_db[ac.icao_addr] = ac;

        // Move to next object
        pos = obj_end + 1;
    }

    return true;
}
