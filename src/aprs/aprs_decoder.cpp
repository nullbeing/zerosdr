#include "aprs/aprs_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace aprs {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kAudioRate = 48000;
constexpr int kBitSamples = 40;   // 1200 baud at 48 kHz
constexpr int kClocks = 8;        // independent bit-clock recoveries
constexpr int kMaxBits = 336 * 8; // generous AX.25 frame limit

std::vector<float> design_lowpass(double sample_rate, double cutoff, int taps) {
    std::vector<float> h(static_cast<size_t>(taps), 0.0f);
    const int m = taps - 1;
    double sum = 0.0;
    const double ratio = 2.0 * cutoff / sample_rate;
    for (int n = 0; n < taps; ++n) {
        const double x = n - m / 2.0;
        const double arg = ratio * x;
        const double sinc = (std::fabs(arg) < 1e-12)
                                ? 1.0
                                : std::sin(kPi * arg) / (kPi * arg);
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * n / m);
        h[static_cast<size_t>(n)] = static_cast<float>(ratio * sinc * window);
        sum += h[static_cast<size_t>(n)];
    }
    if (std::fabs(sum) > 1e-12) {
        for (float& value : h) value = static_cast<float>(value / sum);
    }
    return h;
}

class Fir {
public:
    void init(const std::vector<float>& taps) {
        taps_ = taps;
        history_.assign(taps_.size(), 0.0f);
        position_ = 0;
    }

    float process(float sample) {
        history_[static_cast<size_t>(position_)] = sample;
        position_ = (position_ + 1) % static_cast<int>(history_.size());

        int index = position_ + static_cast<int>(history_.size()) - 1;
        if (index >= static_cast<int>(history_.size())) {
            index -= static_cast<int>(history_.size());
        }

        float sum = 0.0f;
        for (size_t k = 0; k < taps_.size(); ++k) {
            sum += taps_[k] * history_[static_cast<size_t>(index)];
            if (--index < 0) index = static_cast<int>(history_.size()) - 1;
        }
        return sum;
    }

private:
    std::vector<float> taps_;
    std::vector<float> history_;
    int position_ = 0;
};

class ComplexDecimator {
public:
    void init(const std::vector<float>& taps, int decimation) {
        taps_ = taps;
        decimation_ = decimation;
        history_i_.assign(taps_.size(), 0.0f);
        history_q_.assign(taps_.size(), 0.0f);
        position_ = 0;
        counter_ = 0;
    }

    bool process(const std::complex<float>& sample, std::complex<float>& out) {
        history_i_[static_cast<size_t>(position_)] = sample.real();
        history_q_[static_cast<size_t>(position_)] = sample.imag();
        position_ = (position_ + 1) % static_cast<int>(history_i_.size());

        if (++counter_ < decimation_) return false;
        counter_ = 0;

        int index = position_ + static_cast<int>(history_i_.size()) - 1;
        if (index >= static_cast<int>(history_i_.size())) {
            index -= static_cast<int>(history_i_.size());
        }

        float sum_i = 0.0f;
        float sum_q = 0.0f;
        for (size_t k = 0; k < taps_.size(); ++k) {
            const float tap = taps_[k];
            sum_i += tap * history_i_[static_cast<size_t>(index)];
            sum_q += tap * history_q_[static_cast<size_t>(index)];
            if (--index < 0) index = static_cast<int>(history_i_.size()) - 1;
        }
        out = std::complex<float>(sum_i, sum_q);
        return true;
    }

private:
    std::vector<float> taps_;
    std::vector<float> history_i_;
    std::vector<float> history_q_;
    int decimation_ = 1;
    int position_ = 0;
    int counter_ = 0;
};

// Spectral energy of one tone over a sliding window. The complex mixer is
// followed by a boxcar, so the magnitude peaks when the audio contains the
// tone and is near zero otherwise. Comparing the two energies gives the
// Bell 202 mark/space decision.
class ToneDetector {
public:
    void init(double frequency, double sample_rate, int window) {
        window_ = window;
        const double step = 2.0 * kPi * frequency / sample_rate;
        rotation_ = std::complex<float>(static_cast<float>(std::cos(step)),
                                        static_cast<float>(-std::sin(step)));
        oscillator_ = std::complex<float>(1.0f, 0.0f);
        buffer_.assign(static_cast<size_t>(window), std::complex<float>(0.0f, 0.0f));
        position_ = 0;
        sum_ = std::complex<float>(0.0f, 0.0f);
        count_ = 0;
    }

    void push(float sample) {
        const std::complex<float> mixed = sample * oscillator_;
        sum_ += mixed - buffer_[static_cast<size_t>(position_)];
        buffer_[static_cast<size_t>(position_)] = mixed;
        position_ = (position_ + 1) % window_;
        oscillator_ *= rotation_;
        if ((++count_ & 2047) == 0) {
            const float magnitude = std::abs(oscillator_);
            if (magnitude > 1e-6f) oscillator_ /= magnitude;
        }
    }

    float energy() const { return std::norm(sum_); }

private:
    int window_ = 40;
    std::complex<float> rotation_{1.0f, 0.0f};
    std::complex<float> oscillator_{1.0f, 0.0f};
    std::vector<std::complex<float>> buffer_;
    std::complex<float> sum_{0.0f, 0.0f};
    int position_ = 0;
    int count_ = 0;
};

// AX.25 HDLC deframer. Bits arrive LSB-first per byte; bit stuffing is
// removed and the CRC-16/X.25 FCS is validated as 0xF0B8.
class Hdlc {
public:
    bool push(bool bit) {
        if (state_ == State::Hunt) {
            register_ = static_cast<uint8_t>((register_ >> 1) | (bit ? 0x80 : 0));
            if (register_ == 0x7E) {
                state_ = State::Frame;
                position_ = 0;
                ones_ = 0;
                std::memset(data_, 0, sizeof(data_));
            }
            return false;
        }

        // Frame state. Store the bit first, then apply the HDLC rules.
        if (position_ < kMaxBits) {
            if (bit) {
                data_[position_ >> 3] |= static_cast<uint8_t>(1u << (position_ & 7));
            }
            ++position_;
        }

        if (bit) {
            if (ones_ == 5) {
                // Six ones in a row: the closing flag (or an abort). The
                // last seven stored bits belong to the flag, not the frame.
                const int length = position_ - 7;
                const bool ok = validate(length);
                state_ = State::Hunt;
                register_ = 0;
                position_ = 0;
                ones_ = 0;
                if (ok) {
                    bit_length_ = length;
                    return true;
                }
                if (length >= 15 * 8) ++crc_fail_;
                return false;
            }
            ++ones_;
        } else {
            if (ones_ == 5) {
                // Stuffed zero: drop it from the frame.
                if (position_ > 0) --position_;
            }
            ones_ = 0;
        }

        if (position_ >= kMaxBits) {
            state_ = State::Hunt;
            register_ = 0;
            position_ = 0;
            ones_ = 0;
        }
        return false;
    }

    const uint8_t* data() const { return data_; }
    int bit_length() const { return bit_length_; }
    uint64_t crc_fail() const { return crc_fail_; }

private:
    enum class State { Hunt, Frame };

    bool validate(int bits) const {
        if (bits < 15 * 8 || bits > kMaxBits) return false;
        if (bits % 8 != 0) return false;
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < bits; ++i) {
            const uint16_t b =
                static_cast<uint16_t>((data_[i >> 3] >> (i & 7)) & 1u);
            if ((crc ^ b) & 1u) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0x8408u);
            } else {
                crc = static_cast<uint16_t>(crc >> 1);
            }
        }
        return crc == 0xF0B8;
    }

    State state_ = State::Hunt;
    uint8_t register_ = 0;
    int position_ = 0;
    int ones_ = 0;
    int bit_length_ = 0;
    uint64_t crc_fail_ = 0;
    uint8_t data_[kMaxBits / 8]{};
};

// Digital phase-locked loop for the 1200 baud bit clock. A tone change in
// NRZI marks a bit boundary; the loop nudges its phase toward it, so long
// frames follow the transmitter's clock instead of a fixed 40-sample grid.
class ClockRecovery {
public:
    void init(double samples_per_bit, double initial_phase,
              double sample_offset) {
        period_ = samples_per_bit;
        phase_ = initial_phase;
        sample_offset_ = sample_offset;
        sampled_ = false;
        have_level_ = false;
        previous_level_ = false;
        have_bit_ = false;
        previous_bit_level_ = false;
    }

    // Call once per audio sample. Returns true when a bit is ready.
    bool push(bool level, bool& bit) {
        if (have_level_ && level != previous_level_) {
            double error = phase_;
            if (error > period_ / 2.0) error -= period_;
            phase_ -= error * 0.30;
            if (phase_ < 0.0) phase_ += period_;
            if (phase_ >= period_) phase_ -= period_;
        }
        previous_level_ = level;
        have_level_ = true;

        phase_ += 1.0;
        if (phase_ >= period_) {
            phase_ -= period_;
            sampled_ = false;
        }
        if (!sampled_ && phase_ >= sample_offset_) {
            sampled_ = true;
            if (!have_bit_) {
                previous_bit_level_ = level;
                have_bit_ = true;
                return false;
            }
            // NRZI: a transition is a 0, no transition is a 1.
            bit = (level == previous_bit_level_);
            previous_bit_level_ = level;
            return true;
        }
        return false;
    }

private:
    double period_ = 40.0;
    double phase_ = 0.0;
    double sample_offset_ = 20.0;
    bool sampled_ = false;
    bool have_level_ = false;
    bool previous_level_ = false;
    bool have_bit_ = false;
    bool previous_bit_level_ = false;
};

// ---------------------------------------------------------------------------
// APRS information-field parsing
// ---------------------------------------------------------------------------

int base91(char value) {
    const int v = static_cast<unsigned char>(value) - 33;
    if (v < 0 || v > 90) return 0;
    return v;
}

bool is_digit(char value) { return value >= '0' && value <= '9'; }

void parse_weather_fields(Packet& packet, const std::string& text) {
    packet.weather = true;

    // Two encodings appear in the wild: the APRS "cDDD sSSS" weather
    // tokens and the older "DDD/SSS" wind group.
    for (size_t i = 0; i + 4 <= text.size(); ++i) {
        if (!is_digit(text[i]) || !is_digit(text[i + 1]) ||
            !is_digit(text[i + 2]) || text[i + 3] != '/' ||
            !is_digit(text[i + 4])) {
            continue;
        }
        if (i + 7 > text.size()) break;
        if (!is_digit(text[i + 5]) || !is_digit(text[i + 6])) continue;
        packet.wind_dir = static_cast<float>(
            (text[i] - '0') * 100 + (text[i + 1] - '0') * 10 +
            (text[i + 2] - '0'));
        packet.wind_speed_mph = static_cast<float>(
            (text[i + 4] - '0') * 100 + (text[i + 5] - '0') * 10 +
            (text[i + 6] - '0'));
        break;
    }

    for (size_t i = 0; i + 3 < text.size(); ++i) {
        const char tag = text[i];
        if (tag == 't' &&
            (is_digit(text[i + 1]) ||
             (text[i + 1] == '-' && i + 4 < text.size() &&
              is_digit(text[i + 2])))) {
            char* end = nullptr;
            const long fahrenheit = std::strtol(text.c_str() + i + 1, &end, 10);
            if (end != text.c_str() + i + 1) {
                packet.temperature_c = static_cast<float>(
                    (fahrenheit - 32) * 5.0 / 9.0);
            }
        } else if (tag == 'c' && is_digit(text[i + 1]) &&
                   is_digit(text[i + 2]) && is_digit(text[i + 3])) {
            packet.wind_dir = static_cast<float>(
                (text[i + 1] - '0') * 100 + (text[i + 2] - '0') * 10 +
                (text[i + 3] - '0'));
        } else if (tag == 's' && is_digit(text[i + 1]) &&
                   is_digit(text[i + 2]) && is_digit(text[i + 3])) {
            packet.wind_speed_mph = static_cast<float>(
                (text[i + 1] - '0') * 100 + (text[i + 2] - '0') * 10 +
                (text[i + 3] - '0'));
        } else if (tag == 'r' && is_digit(text[i + 1]) &&
                   is_digit(text[i + 2]) && is_digit(text[i + 3])) {
            const int hundredths = (text[i + 1] - '0') * 100 +
                                   (text[i + 2] - '0') * 10 +
                                   (text[i + 3] - '0');
            packet.rain_hour_mm = static_cast<float>(hundredths) * 0.254f;
        }
    }
}

void parse_comment_fields(Packet& packet) {
    const std::string& text = packet.comment;

    // Course/speed: three digits, '/', three digits (e.g. 088/036).
    for (size_t i = 0; i + 7 <= text.size(); ++i) {
        if (!is_digit(text[i]) || !is_digit(text[i + 1]) ||
            !is_digit(text[i + 2]) || text[i + 3] != '/' ||
            !is_digit(text[i + 4]) || !is_digit(text[i + 5]) ||
            !is_digit(text[i + 6])) {
            continue;
        }
        const int course = (text[i] - '0') * 100 + (text[i + 1] - '0') * 10 +
                           (text[i + 2] - '0');
        const int speed = (text[i + 4] - '0') * 100 +
                          (text[i + 5] - '0') * 10 + (text[i + 6] - '0');
        if (course <= 360 && speed <= 999) {
            packet.course = static_cast<float>(course);
            packet.speed_knots = static_cast<float>(speed);
        }
        break;
    }

    // Altitude: /A=001234 (feet).
    const size_t altitude = text.find("/A=");
    if (altitude != std::string::npos) {
        const char* start = text.c_str() + altitude + 3;
        char* end = nullptr;
        const long value = std::strtol(start, &end, 10);
        if (end != start) packet.altitude_ft = static_cast<int>(value);
    }

    // Weather appended to a position comment starts at the last '_'.
    const size_t weather = text.rfind('_');
    if (weather != std::string::npos && weather + 1 < text.size()) {
        parse_weather_fields(packet, text.substr(weather + 1));
    }
}

bool parse_uncompressed(const std::string& info, size_t offset,
                        Packet& packet) {
    if (info.size() < offset + 19) return false;
    const char ns = info[offset + 7];
    const char ew = info[offset + 17];
    if ((ns != 'N' && ns != 'S') || (ew != 'E' && ew != 'W')) return false;

    const std::string lat = info.substr(offset, 7);
    const std::string lon = info.substr(offset + 9, 8);
    if (lat.find('.') == std::string::npos) return false;
    if (lon.find('.') == std::string::npos) return false;

    const double lat_degrees = std::strtod(lat.substr(0, 2).c_str(), nullptr);
    const double lat_minutes = std::strtod(lat.substr(2).c_str(), nullptr);
    const double lon_degrees = std::strtod(lon.substr(0, 3).c_str(), nullptr);
    const double lon_minutes = std::strtod(lon.substr(3).c_str(), nullptr);
    if (lat_minutes >= 60.0 || lon_minutes >= 60.0) return false;

    double latitude = lat_degrees + lat_minutes / 60.0;
    double longitude = lon_degrees + lon_minutes / 60.0;
    if (ns == 'S') latitude = -latitude;
    if (ew == 'W') longitude = -longitude;
    if (latitude < -90.0 || latitude > 90.0) return false;
    if (longitude < -180.0 || longitude > 180.0) return false;

    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.symbol_table = info[offset + 8];
    packet.symbol = info[offset + 18];
    packet.comment = info.substr(offset + 19);
    packet.has_position = true;
    parse_comment_fields(packet);
    return true;
}

bool parse_compressed(const std::string& info, size_t offset, Packet& packet) {
    if (info.size() < offset + 13) return false;

    long lat_value = 0;
    long lon_value = 0;
    for (int i = 0; i < 4; ++i) {
        lat_value = lat_value * 91 + base91(info[offset + 1 + i]);
        lon_value = lon_value * 91 + base91(info[offset + 5 + i]);
    }

    const double latitude = 90.0 - static_cast<double>(lat_value) / 380926.0;
    const double longitude = -180.0 + static_cast<double>(lon_value) / 190463.0;
    if (latitude < -90.0 || latitude > 90.0) return false;
    if (longitude < -180.0 || longitude > 180.0) return false;

    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.symbol_table = info[offset];
    packet.symbol = info[offset + 9];

    const int c = base91(info[offset + 10]);
    const int s = base91(info[offset + 11]);
    const char compression = info[offset + 12];
    if (compression == 'G') {
        const double feet = std::pow(1.002, c * 91 + s);
        packet.altitude_ft = static_cast<int>(std::lround(feet));
    } else if (compression == ' ') {
        packet.course = static_cast<float>(c * 4);
        packet.speed_knots = static_cast<float>(std::pow(1.08, s) - 1.0);
    }

    packet.comment = info.substr(offset + 13);
    packet.has_position = true;
    parse_comment_fields(packet);
    return true;
}

bool looks_compressed(const std::string& info, size_t offset) {
    if (offset >= info.size()) return false;
    const char value = info[offset];
    if (value == '/' || value == '\\') return true;
    if (value >= 'A' && value <= 'Z') return true;  // overlay table
    return false;
}

void parse_mic_e(Packet& packet) {
    const std::string& info = packet.info;
    if (info.size() < 9) return;  // DTI + 8 data bytes

    std::string destination = packet.destination;
    const size_t dash = destination.find('-');
    if (dash != std::string::npos) destination.resize(dash);
    if (destination.size() < 6) return;

    char digits[6];
    for (int i = 0; i < 6; ++i) {
        const char value = destination[static_cast<size_t>(i)];
        if (value == 'K' || value == 'L' || value == 'Z') {
            digits[i] = '0';  // position ambiguity: treat as the box corner
        } else if (value >= 'P' && value <= 'Y') {
            digits[i] = static_cast<char>('0' + (value - 'P'));
        } else if (value >= 'A' && value <= 'J') {
            digits[i] = static_cast<char>('0' + (value - 'A'));
        } else if (value >= '0' && value <= '9') {
            digits[i] = value;
        } else {
            return;
        }
    }

    const int lat_degrees = (digits[0] - '0') * 10 + (digits[1] - '0');
    const int lat_minutes = (digits[2] - '0') * 10 + (digits[3] - '0');
    const int lat_hundredths = (digits[4] - '0') * 10 + (digits[5] - '0');
    if (lat_degrees > 90 || lat_minutes > 59 || lat_hundredths > 99) return;

    double latitude = lat_degrees +
                      (lat_minutes + lat_hundredths / 100.0) / 60.0;
    if (destination[3] <= 'L') latitude = -latitude;

    int degrees = static_cast<unsigned char>(info[1]) - 28;
    if (destination[4] >= 'P') degrees += 100;
    if (degrees >= 180 && degrees <= 189) {
        degrees -= 80;
    } else if (degrees >= 190 && degrees <= 199) {
        degrees -= 190;
    }

    int minutes = static_cast<unsigned char>(info[2]) - 28;
    if (minutes >= 60) minutes -= 60;
    const int hundredths = static_cast<unsigned char>(info[3]) - 28;
    if (degrees < 0 || degrees > 180 || minutes < 0 || minutes > 59 ||
        hundredths < 0 || hundredths > 99) {
        return;
    }

    double longitude =
        degrees + (minutes + hundredths / 100.0) / 60.0;
    if (destination[5] >= 'P') longitude = -longitude;

    int speed = (static_cast<unsigned char>(info[4]) - 28) * 10 +
                (static_cast<unsigned char>(info[5]) - 28) / 10;
    int course = ((static_cast<unsigned char>(info[5]) - 28) % 10) * 100 +
                 (static_cast<unsigned char>(info[6]) - 28);
    if (speed >= 800) speed -= 800;
    if (course >= 400) course -= 400;
    if (speed < 0 || speed > 999 || course < 0 || course > 360) return;

    packet.latitude = latitude;
    packet.longitude = longitude;
    packet.speed_knots = static_cast<float>(speed);
    packet.course = static_cast<float>(course);
    packet.symbol = info[7];
    packet.symbol_table = info[8];
    if (info.size() > 9) packet.comment = info.substr(9);
    packet.has_position = true;
    parse_comment_fields(packet);
}

void parse_message(Packet& packet, const std::string& info) {
    if (info.size() < 11 || info[10] != ':') return;
    std::string addressee = info.substr(1, 9);
    while (!addressee.empty() && addressee.back() == ' ') {
        addressee.pop_back();
    }
    std::string text = info.substr(11);
    const size_t number = text.rfind('{');
    if (number != std::string::npos) text = text.substr(0, number);
    packet.message = true;
    packet.addressee = addressee;
    packet.message_text = text;
}

void parse_weather(Packet& packet, const std::string& info) {
    // _MMDDHHMM followed by the weather fields.
    const std::string fields = info.size() > 8 ? info.substr(8)
                                               : std::string();
    parse_weather_fields(packet, fields);
}

void parse_info(Packet& packet) {
    const std::string& info = packet.info;
    if (info.empty()) return;
    packet.type = info[0];

    switch (info[0]) {
        case '!':
        case '=': {
            const size_t offset = 1;
            if (looks_compressed(info, offset)) {
                parse_compressed(info, offset, packet);
            } else {
                parse_uncompressed(info, offset, packet);
            }
            break;
        }
        case '/':
        case '@': {
            // Position with a 7-character timestamp.
            if (info.size() < 8) break;
            const size_t offset = 8;
            if (looks_compressed(info, offset)) {
                parse_compressed(info, offset, packet);
            } else {
                parse_uncompressed(info, offset, packet);
            }
            break;
        }
        case '\'':
        case '`':
        case '\x1c':
        case '\x1d':
            parse_mic_e(packet);
            break;
        case ':':
            parse_message(packet, info);
            break;
        case '>':
            packet.comment = info.substr(1);
            break;
        case '_':
            parse_weather(packet, info);
            break;
        default:
            break;
    }
}

void parse_ax25(const uint8_t* frame, size_t bytes, Packet& packet) {
    if (bytes < 18) return;

    size_t position = 0;
    std::vector<std::string> addresses;
    while (position + 7 <= bytes && addresses.size() < 10) {
        std::string call;
        for (int i = 0; i < 6; ++i) {
            const char value = static_cast<char>(frame[position + i] >> 1);
            if (value != ' ' && value >= 0x20 && value < 0x7F) {
                call.push_back(value);
            }
        }
        const uint8_t ssid_byte = frame[position + 6];
        const int ssid = (ssid_byte >> 1) & 0x0F;
        if (ssid != 0 && !call.empty()) {
            call += "-" + std::to_string(ssid);
        }
        addresses.push_back(call);
        const bool last = (ssid_byte & 1u) != 0;
        position += 7;
        if (last) break;
    }
    if (addresses.size() < 2 || position + 2 > bytes) return;

    const uint8_t control = frame[position++];
    // UI frames (0x03, optionally with the poll bit) carry APRS.
    if ((control & 0xEF) != 0x03) return;
    const uint8_t pid = frame[position++];
    (void)pid;

    const size_t info_start = position;
    if (info_start + 2 > bytes) return;
    const size_t info_end = bytes - 2;  // drop the FCS
    if (info_end <= info_start) return;

    packet.destination = addresses[0];
    packet.source = addresses[1];
    for (size_t i = 2; i < addresses.size(); ++i) {
        packet.path.push_back(addresses[i]);
    }
    packet.info.assign(reinterpret_cast<const char*>(frame + info_start),
                       info_end - info_start);
    // Parse before sanitizing: Mic-E carries unprintable bytes in its
    // longitude/speed fields.
    parse_info(packet);
    for (char& value : packet.info) {
        if (value == '\r' || value == '\n') value = ' ';
        if (static_cast<unsigned char>(value) < 0x20) value = '.';
    }
    for (char& value : packet.comment) {
        if (static_cast<unsigned char>(value) < 0x20) value = ' ';
    }
}

}  // namespace

struct Decoder::Impl {
    Config config;
    std::function<void(const Packet&)> callback;

    ComplexDecimator stage1;
    ComplexDecimator stage2;
    Fir audio_filter;

    std::complex<float> previous_iq{0.0f, 0.0f};
    bool have_previous = false;

    float dc_input = 0.0f;
    float dc_output = 0.0f;
    float agc = 0.0f;
    float raw_dc_input = 0.0f;
    float raw_dc_output = 0.0f;

    ToneDetector mark;
    ToneDetector space;

    ClockRecovery clocks[kClocks];
    Hdlc hdlc[kClocks];
    float clock_gain[kClocks]{};

    Stats stats;

    struct Recent {
        uint64_t hash;
        double timestamp;
    };
    std::deque<Recent> recent;

    void on_audio(float sample);
    void on_frame(const uint8_t* frame, size_t bytes);
    void on_bits(int clock, bool bit);
};

void Decoder::Impl::on_audio(float sample) {
    mark.push(sample);
    space.push(sample);
    const float mark_energy = mark.energy();
    const float space_energy = space.energy();

    for (int clock = 0; clock < kClocks; ++clock) {
        // The gain bank absorbs the mark/space imbalance caused by receiver
        // de-emphasis and by a non-flat noise floor.
        const bool level = mark_energy * clock_gain[clock] > space_energy;
        bool bit = false;
        if (clocks[clock].push(level, bit)) {
            on_bits(clock, bit);
        }
    }
}

void Decoder::Impl::on_bits(int clock, bool bit) {
    ++stats.bits;
    if (!bit) ++stats.transitions;

    Hdlc& framer = hdlc[clock];
    if (framer.push(bit)) {
        const int bits = framer.bit_length();
        if (bits % 8 == 0) {
            on_frame(framer.data(), static_cast<size_t>(bits / 8));
        }
    }
}

void Decoder::Impl::on_frame(const uint8_t* frame, size_t bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= frame[i];
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<uint64_t>(bytes) * 0x9E3779B97F4A7C15ULL;

    const double timestamp =
        static_cast<double>(stats.samples) / config.sample_rate;
    for (const Recent& entry : recent) {
        if (entry.hash == hash && timestamp - entry.timestamp < 1.0) {
            ++stats.duplicates;
            return;
        }
    }
    recent.push_back({hash, timestamp});
    while (recent.size() > 64 ||
           (!recent.empty() && timestamp - recent.front().timestamp > 3.0)) {
        recent.pop_front();
    }

    Packet packet;
    parse_ax25(frame, bytes, packet);
    if (packet.source.empty()) return;

    ++stats.packets;
    if (callback) callback(packet);
}

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}

Decoder::~Decoder() = default;

bool Decoder::init(const Config& config) {
    // The ten-phase sampler assumes exactly 40 audio samples per bit and a
    // 25:1 decimation from the capture rate.
    if (std::fabs(config.sample_rate - 1200000.0) > 1.0) return false;
    if (config.audio_rate != kAudioRate) return false;

    impl_->config = config;
    impl_->stage1.init(design_lowpass(config.sample_rate, 30000.0, 31), 5);
    impl_->stage2.init(design_lowpass(240000.0, 5000.0, 31), 5);
    impl_->audio_filter.init(design_lowpass(kAudioRate, 4000.0, 31));
    impl_->mark.init(1200.0, kAudioRate, kBitSamples);
    impl_->space.init(2200.0, kAudioRate, kBitSamples);

    impl_->previous_iq = std::complex<float>(0.0f, 0.0f);
    impl_->have_previous = false;
    impl_->dc_input = 0.0f;
    impl_->dc_output = 0.0f;
    impl_->agc = 0.0f;
    impl_->raw_dc_input = 0.0f;
    impl_->raw_dc_output = 0.0f;
    const double sample_offsets[kClocks] = {
        18.0, 20.0, 18.0, 20.0, 18.0, 20.0, 18.0, 20.0};
    const float gains[kClocks] = {
        0.55f, 0.55f, 0.70f, 0.70f, 0.85f, 0.85f, 1.00f, 1.00f};
    for (int i = 0; i < kClocks; ++i) {
        impl_->clocks[i].init(kBitSamples, 0.0, sample_offsets[i]);
        impl_->clock_gain[i] = gains[i];
        impl_->hdlc[i] = Hdlc();
    }
    impl_->recent.clear();
    impl_->stats = Stats{};
    return true;
}

void Decoder::reset() {
    const auto callback = impl_->callback;
    Config config = impl_->config;
    init(config);
    impl_->callback = callback;
}

void Decoder::process(const uint8_t* cu8, size_t sample_count) {
    if (cu8 == nullptr || sample_count == 0) return;
    constexpr float kScale = 1.0f / 127.5f;

    for (size_t n = 0; n < sample_count; ++n) {
        const float i = (static_cast<float>(cu8[2 * n]) - 127.5f) * kScale;
        const float q = (static_cast<float>(cu8[2 * n + 1]) - 127.5f) * kScale;

        std::complex<float> first;
        if (!impl_->stage1.process(std::complex<float>(i, q), first)) {
            ++impl_->stats.samples;
            continue;
        }
        std::complex<float> second;
        if (!impl_->stage2.process(first, second)) {
            ++impl_->stats.samples;
            continue;
        }

        if (!impl_->have_previous) {
            impl_->previous_iq = second;
            impl_->have_previous = true;
            ++impl_->stats.samples;
            continue;
        }

        const std::complex<float> product =
            second * std::conj(impl_->previous_iq);
        impl_->previous_iq = second;
        const float discriminator = std::atan2(product.imag(), product.real()) /
                                    static_cast<float>(kPi);

        // Remove the constant tuning offset before the audio filter.
        const float dc =
            discriminator - impl_->dc_input + 0.999f * impl_->dc_output;
        impl_->dc_input = discriminator;
        impl_->dc_output = dc;

        const float filtered = impl_->audio_filter.process(dc);
        impl_->agc = 0.999f * impl_->agc + 0.001f * std::fabs(filtered);
        const float normalized = filtered / std::max(impl_->agc, 1e-4f);
        impl_->on_audio(normalized);

        ++impl_->stats.samples;
    }
}

void Decoder::processAudio(const float* samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0) return;
    for (size_t n = 0; n < sample_count; ++n) {
        const float sample = samples[n];
        const float dc =
            sample - impl_->raw_dc_input + 0.999f * impl_->raw_dc_output;
        impl_->raw_dc_input = sample;
        impl_->raw_dc_output = dc;
        const float filtered = impl_->audio_filter.process(dc);
        impl_->agc = 0.999f * impl_->agc + 0.001f * std::fabs(filtered);
        impl_->on_audio(filtered / std::max(impl_->agc, 1e-4f));
        // Keep the timestamp scale equivalent to the IQ path (25:1).
        impl_->stats.samples += 25;
    }
}

void Decoder::setPacketCallback(std::function<void(const Packet&)> callback) {
    impl_->callback = std::move(callback);
}

Decoder::Stats Decoder::stats() const {
    Stats result = impl_->stats;
    for (int i = 0; i < kClocks; ++i) {
        result.crc_fail += impl_->hdlc[i].crc_fail();
    }
    return result;
}

const Decoder::Config& Decoder::config() const { return impl_->config; }

}  // namespace aprs
