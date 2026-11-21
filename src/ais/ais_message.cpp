#include "ais/ais_message.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ais {
namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, int bit_length)
        : data_(data), bit_length_(bit_length) {}

    uint32_t get(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | bit(pos_++);
        }
        return value;
    }

    int64_t getSigned(int count) {
        uint32_t value = get(count);
        if (count < 32 && (value & (1u << (count - 1))) != 0) {
            value |= ~((1u << count) - 1u);
        }
        return static_cast<int32_t>(value);
    }

    int remaining() const { return bit_length_ - pos_; }
    int position() const { return pos_; }

private:
    bool bit(int index) const {
        if (index < 0 || index >= bit_length_) return false;
        return (data_[index >> 3] >> (7 - (index & 7))) & 1;
    }

    const uint8_t* data_;
    int bit_length_;
    int pos_ = 0;
};

std::string trimSixBit(std::string value) {
    while (!value.empty() && (value.back() == '@' || value.back() == ' ')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && value[start] == ' ') ++start;
    value.erase(0, start);
    for (char& c : value) {
        if (static_cast<unsigned char>(c) < 32) c = ' ';
    }
    return value;
}

std::string readSixBitString(BitReader& reader, int bit_length) {
    const int count = bit_length / 6;
    std::string value;
    value.reserve(count);
    for (int i = 0; i < count; ++i) {
        value.push_back(sixBitToChar(static_cast<uint8_t>(reader.get(6))));
    }
    return trimSixBit(std::move(value));
}

bool validLongitude(int64_t raw) {
    return raw >= -180LL * 600000 && raw <= 180LL * 600000;
}

bool validLatitude(int64_t raw) {
    return raw >= -90LL * 600000 && raw <= 90LL * 600000;
}

void readPosition(BitReader& reader, AisMessage& out) {
    const int64_t lon_raw = reader.getSigned(28);
    const int64_t lat_raw = reader.getSigned(27);
    if (validLongitude(lon_raw) && validLatitude(lat_raw)) {
        out.longitude = static_cast<double>(lon_raw) / 600000.0;
        out.latitude = static_cast<double>(lat_raw) / 600000.0;
        out.has_position = true;
    }
}

void readDimensions(BitReader& reader, AisMessage& out) {
    out.to_bow = static_cast<int>(reader.get(9));
    out.to_stern = static_cast<int>(reader.get(9));
    out.to_port = static_cast<int>(reader.get(6));
    out.to_starboard = static_cast<int>(reader.get(6));
}

void readDynamicClassA(BitReader& reader, AisMessage& out) {
    out.nav_status = static_cast<int>(reader.get(4));

    const int rot_raw = static_cast<int>(reader.getSigned(8));
    if (rot_raw != -128) {
        const float magnitude = static_cast<float>(rot_raw) / 4.733f;
        out.rot = std::copysign(magnitude * magnitude, static_cast<float>(rot_raw));
        out.rot_valid = true;
    }

    const int sog_raw = static_cast<int>(reader.get(10));
    if (sog_raw != 1023) out.sog = sog_raw / 10.0f;

    reader.get(1);  // position accuracy
    readPosition(reader, out);

    const int cog_raw = static_cast<int>(reader.get(12));
    if (cog_raw < 3600) out.cog = cog_raw / 10.0f;

    const int heading_raw = static_cast<int>(reader.get(9));
    if (heading_raw < 360) out.heading = heading_raw;

    const int utc = static_cast<int>(reader.get(6));
    if (utc < 60) out.utc_second = utc;

    reader.get(2);  // maneuver
    reader.get(3);  // spare
    reader.get(1);  // RAIM
    reader.get(19); // radio status
}

}  // namespace

char sixBitToChar(uint8_t value) {
    if (value < 32) return static_cast<char>(value + 64);
    return static_cast<char>(value);
}

std::string sixBitString(const uint8_t* data, int bit_offset, int bit_length) {
    std::string value;
    value.reserve(bit_length / 6);
    for (int i = 0; i + 6 <= bit_length; i += 6) {
        uint8_t code = 0;
        for (int b = 0; b < 6; ++b) {
            const int index = bit_offset + i + b;
            code = static_cast<uint8_t>((code << 1) |
                ((data[index >> 3] >> (7 - (index & 7))) & 1));
        }
        value.push_back(sixBitToChar(code));
    }
    return trimSixBit(std::move(value));
}

void AisMessage::clear() {
    *this = AisMessage{};
}

bool parseAisMessage(const AisFrame& frame, AisMessage& out) {
    out.clear();
    if (frame.bit_length < 16 + 38) return false;

    // The HDLC frame carries the AIS bits least-significant bit first inside
    // each byte. Reverse every byte so the field reader sees the normal
    // MSB-first AIS layout (the CRC is checked on the unreversed frame).
    std::array<uint8_t, 140> field_bits{};
    for (size_t i = 0; i < field_bits.size(); ++i) {
        uint8_t value = frame.bits[i];
        uint8_t reversed = 0;
        for (int b = 0; b < 8; ++b) {
            reversed = static_cast<uint8_t>((reversed << 1) | (value & 1u));
            value >>= 1;
        }
        field_bits[i] = reversed;
    }

    const int data_length = frame.bit_length - 16;
    BitReader reader(field_bits.data(), data_length);

    out.type = static_cast<int>(reader.get(6));
    out.repeat = static_cast<int>(reader.get(2));
    out.mmsi = reader.get(30);

    if (out.type < 1 || out.type > 27 || out.mmsi == 0) return false;

    switch (out.type) {
        case 1:
        case 2:
        case 3: {
            readDynamicClassA(reader, out);
            break;
        }

        case 4: {
            out.is_base_station = true;
            const int year = static_cast<int>(reader.get(14));
            const int month = static_cast<int>(reader.get(4));
            const int day = static_cast<int>(reader.get(5));
            const int hour = static_cast<int>(reader.get(5));
            const int minute = static_cast<int>(reader.get(6));
            const int second = static_cast<int>(reader.get(6));
            if (year >= 1 && month >= 1 && month <= 12 &&
                day >= 1 && day <= 31 && hour < 24 && minute < 60) {
                out.utc_year = year;
                out.utc_month = month;
                out.utc_day = day;
                out.utc_hour = hour;
                out.utc_minute = minute;
            }
            if (second < 60) out.utc_second = second;
            reader.get(1);  // accuracy
            readPosition(reader, out);
            out.epfd = static_cast<int>(reader.get(4));
            break;
        }

        case 5: {
            reader.get(2);  // AIS version
            out.imo = reader.get(30);
            out.callsign = readSixBitString(reader, 42);
            out.name = readSixBitString(reader, 120);
            out.ship_type = static_cast<int>(reader.get(8));
            readDimensions(reader, out);
            out.epfd = static_cast<int>(reader.get(4));

            out.eta_month = static_cast<int>(reader.get(4));
            out.eta_day = static_cast<int>(reader.get(5));
            out.eta_hour = static_cast<int>(reader.get(5));
            out.eta_minute = static_cast<int>(reader.get(6));
            if (out.eta_month > 12 || out.eta_hour > 23 || out.eta_minute > 59) {
                out.eta_month = out.eta_day = out.eta_hour = out.eta_minute = 0;
            }

            out.draught = reader.get(8) / 10.0f;
            out.destination = readSixBitString(reader, 120);
            break;
        }

        case 18: {
            out.class_b = true;
            reader.get(8);  // reserved
            const int sog_raw = static_cast<int>(reader.get(10));
            if (sog_raw != 1023) out.sog = sog_raw / 10.0f;
            reader.get(1);  // accuracy
            readPosition(reader, out);
            const int cog_raw = static_cast<int>(reader.get(12));
            if (cog_raw < 3600) out.cog = cog_raw / 10.0f;
            const int heading_raw = static_cast<int>(reader.get(9));
            if (heading_raw < 360) out.heading = heading_raw;
            const int utc = static_cast<int>(reader.get(6));
            if (utc < 60) out.utc_second = utc;
            break;
        }

        case 19: {
            out.class_b = true;
            reader.get(8);  // reserved
            const int sog_raw = static_cast<int>(reader.get(10));
            if (sog_raw != 1023) out.sog = sog_raw / 10.0f;
            reader.get(1);  // accuracy
            readPosition(reader, out);
            const int cog_raw = static_cast<int>(reader.get(12));
            if (cog_raw < 3600) out.cog = cog_raw / 10.0f;
            const int heading_raw = static_cast<int>(reader.get(9));
            if (heading_raw < 360) out.heading = heading_raw;
            const int utc = static_cast<int>(reader.get(6));
            if (utc < 60) out.utc_second = utc;
            reader.get(4);  // reserved
            out.name = readSixBitString(reader, 120);
            out.ship_type = static_cast<int>(reader.get(8));
            readDimensions(reader, out);
            out.epfd = static_cast<int>(reader.get(4));
            break;
        }

        case 21: {
            out.is_aton = true;
            reader.get(5);  // aid-to-navigation type
            out.name = readSixBitString(reader, 120);
            readDimensions(reader, out);
            out.epfd = static_cast<int>(reader.get(4));
            const int utc = static_cast<int>(reader.get(6));
            if (utc < 60) out.utc_second = utc;
            out.off_position = reader.get(1) != 0;
            reader.get(8);  // regional
            reader.get(1);  // RAIM
            out.virtual_aton = reader.get(1) != 0;
            break;
        }

        case 24: {
            const int part = static_cast<int>(reader.get(2));
            if (part == 0) {
                out.name = readSixBitString(reader, 120);
            } else {
                out.ship_type = static_cast<int>(reader.get(8));
                reader.get(18);  // vendor ID
                out.callsign = readSixBitString(reader, 42);
                if (reader.remaining() >= 30) readDimensions(reader, out);
            }
            break;
        }

        case 27: {
            reader.get(1);  // accuracy
            reader.get(1);  // RAIM
            out.nav_status = static_cast<int>(reader.get(4));
            const int64_t lon_raw = reader.getSigned(18);
            const int64_t lat_raw = reader.getSigned(17);
            if (lon_raw >= -180LL * 600 && lon_raw <= 180LL * 600 &&
                lat_raw >= -90LL * 600 && lat_raw <= 90LL * 600) {
                out.longitude = static_cast<double>(lon_raw) / 600.0;
                out.latitude = static_cast<double>(lat_raw) / 600.0;
                out.has_position = true;
            }
            const int sog_raw = static_cast<int>(reader.get(6));
            if (sog_raw != 63) out.sog = static_cast<float>(sog_raw);
            const int cog_raw = static_cast<int>(reader.get(9));
            if (cog_raw < 360) out.cog = static_cast<float>(cog_raw);
            break;
        }

        default:
            break;
    }

    out.valid = true;
    return true;
}

}  // namespace ais
