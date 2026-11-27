#include "acars/acars_parser.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace acars {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isUpperAlnum(char c) { return isUpper(c) || isDigit(c); }

bool allDigits(const std::string& text, size_t start, size_t length) {
    if (length == 0 || start + length > text.size()) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!isDigit(text[start + i])) return false;
    }
    return true;
}

// ICAO airport codes are four upper-case alphanumerics ("ZGGG", "KJFK").
bool isAirport(const std::string& text) {
    if (text.size() != 4 || !isUpper(text[0])) return false;
    for (char c : text) {
        if (!isUpperAlnum(c)) return false;
    }
    return true;
}

std::string firstLine(const std::string& text) {
    const size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

struct Coordinate {
    bool ok = false;
    double value = 0.0;
};

/**
 * Reads the number following an N/S/E/W letter. ACARS position reports use
 * three different encodings:
 *   decimal degrees   24.448
 *   degrees * 1e6     23028576   (lat), 113248512 (lon)
 *   DDMMSS            230285
 * A bare one to three digit number is ambiguous ("CAS 280", "W 5") and is
 * rejected rather than guessed.
 */
Coordinate parseCoordinate(const std::string& text, size_t letter) {
    Coordinate out;
    size_t i = letter + 1;
    while (i < text.size() && text[i] == ' ') ++i;

    const size_t start = i;
    size_t digits = 0;
    bool dot = false;
    while (i < text.size()) {
        const char c = text[i];
        if (isDigit(c)) {
            ++digits;
            ++i;
        } else if (c == '.' && !dot) {
            dot = true;
            ++i;
        } else {
            break;
        }
    }
    if (digits == 0) return out;
    // Reject numbers glued to a word ("N123ABC") or a second decimal point.
    if (i < text.size() && (isUpperAlnum(text[i]) || text[i] == '.')) {
        return out;
    }

    const std::string number = text.substr(start, i - start);
    if (dot) {
        if (digits > 11) return out;
        out.value = std::strtod(number.c_str(), nullptr);
    } else if (digits >= 7) {
        // Degrees scaled by 1e6, the common ARINC 620 position report form.
        out.value = std::strtod(number.c_str(), nullptr) / 1e6;
    } else if (digits == 6) {
        const long v = std::strtol(number.c_str(), nullptr, 10);
        out.value = static_cast<double>(v / 10000) +
                    static_cast<double>((v / 100) % 100) / 60.0 +
                    static_cast<double>(v % 100) / 3600.0;
    } else if (digits == 4) {
        const long v = std::strtol(number.c_str(), nullptr, 10);
        out.value = static_cast<double>(v / 100) +
                    static_cast<double>(v % 100) / 60.0;
    } else {
        return out;
    }
    out.ok = true;
    return out;
}

bool findPosition(const std::string& text, double& latitude, double& longitude) {
    bool have_lat = false;
    bool have_lon = false;
    for (size_t i = 0; i < text.size(); ++i) {
        // Only start at a token boundary, so letters inside words ("POSITION",
        // "SORRY") are never mistaken for a hemisphere marker.
        if (i > 0 && isUpperAlnum(text[i - 1])) continue;
        const char c = text[i];
        if (c != 'N' && c != 'S' && c != 'E' && c != 'W') continue;
        const Coordinate coord = parseCoordinate(text, i);
        if (!coord.ok) continue;
        if (c == 'N' || c == 'S') {
            if (have_lat || coord.value > 90.0) continue;
            latitude = (c == 'S') ? -coord.value : coord.value;
            have_lat = true;
        } else {
            if (have_lon || coord.value > 180.0) continue;
            longitude = (c == 'W') ? -coord.value : coord.value;
            have_lon = true;
        }
    }
    if (!have_lat || !have_lon) return false;
    return std::fabs(latitude) > 0.0001 || std::fabs(longitude) > 0.0001;
}

void splitFields(const std::string& text, std::vector<std::string>& out) {
    size_t start = 0;
    while (out.size() < 12) {
        const size_t comma = text.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(text.substr(start));
            return;
        }
        out.push_back(text.substr(start, comma - start));
        start = comma + 1;
    }
}

void parseAltitude(const std::string& text, Decoded& out) {
    size_t pos = 0;
    while ((pos = text.find("ALT", pos)) != std::string::npos) {
        size_t i = pos + 3;
        while (i < text.size() && (text[i] == ' ' || text[i] == ':')) ++i;
        const size_t start = i;
        while (i < text.size() && isDigit(text[i])) ++i;
        if (i > start && i - start <= 5) {
            out.altitude_ft =
                std::atoi(text.substr(start, i - start).c_str());
            out.has_altitude = true;
            return;
        }
        pos += 3;
    }
}

void parseUtcTime(const std::string& text, Decoded& out) {
    if (!out.report_time.empty()) return;
    const size_t at = text.find("UTC ");
    if (at == std::string::npos) return;
    if (!allDigits(text, at + 4, 6)) return;
    out.report_time = text.substr(at + 4, 4) + "Z";
}

}  // namespace

const char* kindTag(Kind kind) {
    switch (kind) {
        case Kind::Position: return "POS";
        case Kind::Ats: return "ATS";
        case Kind::Adsc: return "ADSC";
        case Kind::System: return "SYS";
        case Kind::FreeText: return "TXT";
        case Kind::None:
        default: return "---";
    }
}

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Position: return "POSITION REPORT";
        case Kind::Ats: return "ATS DATALINK";
        case Kind::Adsc: return "ADS-C CONTRACT";
        case Kind::System: return "SYSTEM ADVISORY";
        case Kind::FreeText: return "FREE TEXT";
        case Kind::None:
        default: return "NO TEXT";
    }
}

Decoded decodeText(const Message& message) {
    Decoded out;
    const std::string& text = message.text;
    if (text.empty()) return out;  // Kind::None

    out.kind = Kind::FreeText;

    // ADS-C payloads are binary hex carried as text; there is nothing useful
    // to show without a contract database.
    if (message.label == "B6" || text.find(".ADS.") != std::string::npos) {
        out.kind = Kind::Adsc;
        return out;
    }

    // ARINC 622 ATS messages start with '#' and a short type code.
    if (text[0] == '#' && text.size() >= 3) {
        size_t i = 1;
        while (i < text.size() && i < 4 && isUpperAlnum(text[i])) ++i;
        if (i > 2) {
            out.kind = Kind::Ats;
            out.subtype = text.substr(1, i - 1);
            return out;
        }
    }

    // Label SA media/system advisory: "0" + media code + HHMMSS + flags.
    if (message.label == "SA" && text.size() >= 9 && text[0] == '0' &&
        isUpper(text[1]) && isUpper(text[2]) && allDigits(text, 3, 6)) {
        out.kind = Kind::System;
        out.subtype = text.substr(1, 2);
        char when[16];
        std::snprintf(when, sizeof(when), "%c%c:%c%c:%c%cZ", text[3], text[4],
                      text[5], text[6], text[7], text[8]);
        out.report_time = when;
        return out;
    }

    const std::string line = firstLine(text);

    // ARINC 620 "POX" position report:
    // POX,flight,DDMMMYY,HHMMSS,origin,dest,lat,lon,...
    if (line.rfind("POX,", 0) == 0) {
        std::vector<std::string> fields;
        splitFields(line, fields);
        if (fields.size() >= 6) {
            if (isAirport(fields[4])) out.origin = fields[4];
            if (isAirport(fields[5])) out.destination = fields[5];
            if (allDigits(fields[3], 0, 6)) {
                out.report_time = fields[3].substr(0, 4) + "Z";
            }
        }
    } else if (line.size() >= 10 && line[4] == '/' && line[9] == '/' &&
               isAirport(line.substr(0, 4)) &&
               isAirport(line.substr(5, 4))) {
        // Slash-separated report: ORIG/DEST/date/...
        out.origin = line.substr(0, 4);
        out.destination = line.substr(5, 4);
    }

    double latitude = 0.0;
    double longitude = 0.0;
    if (findPosition(text, latitude, longitude)) {
        out.kind = Kind::Position;
        out.has_position = true;
        out.latitude = latitude;
        out.longitude = longitude;
    }

    parseAltitude(text, out);
    parseUtcTime(text, out);
    return out;
}

bool isRepeatedFrame(const Message& held, const Message& incoming) {
    if (held.msgno.empty() || incoming.msgno.empty()) return false;
    if (held.msgno != incoming.msgno) return false;
    if (held.flight != incoming.flight) return false;
    if (held.label != incoming.label) return false;
    if (held.text != incoming.text) return false;
    return std::fabs(held.timestamp - incoming.timestamp) <= 1800.0;
}

bool isContinuationBlockOf(const Message& base, const Message& part) {
    if (base.msgno.size() != 4 || part.msgno.size() != 4) return false;
    if (base.flight != part.flight || base.label != part.label) return false;
    if (base.msgno.compare(0, 3, part.msgno, 0, 3) != 0) return false;
    const char next = part.msgno[3];
    if (next <= base.last_block || next > 'Z') return false;
    if (part.text.empty()) return false;
    return std::fabs(base.timestamp - part.timestamp) <= 300.0;
}

bool appendContinuation(Message& base, const Message& part) {
    if (!isContinuationBlockOf(base, part)) return false;
    // Keep the merged text bounded; the controller stores at most 2000 chars
    // per frame and a runaway chain must not grow without limit.
    constexpr size_t kMaxMergedText = 2000;
    if (base.text.size() < kMaxMergedText) {
        const size_t room = kMaxMergedText - base.text.size();
        base.text.append(part.text, 0, room);
    }
    base.last_block = part.msgno[3];
    if (base.block_count < 255) ++base.block_count;
    if (part.timestamp > base.timestamp) base.timestamp = part.timestamp;
    return true;
}

bool isMergedBlock(const Message& base, const Message& part) {
    if (base.msgno.size() != 4 || part.msgno.size() != 4) return false;
    if (base.msgno.compare(0, 3, part.msgno, 0, 3) != 0) return false;
    if (base.flight != part.flight || base.label != part.label) return false;
    if (part.msgno[3] > base.last_block) return false;
    if (part.text.empty() || base.text.size() < part.text.size()) return false;
    return base.text.compare(base.text.size() - part.text.size(),
                             part.text.size(), part.text) == 0;
}

}  // namespace acars
