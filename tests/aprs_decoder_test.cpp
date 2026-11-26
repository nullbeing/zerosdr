#include "aprs/aprs_decoder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<uint8_t> make_address(const std::string& call, int ssid,
                                  bool last) {
    std::vector<uint8_t> out(7, static_cast<uint8_t>(' ' << 1));
    for (size_t i = 0; i < call.size() && i < 6; ++i) {
        out[i] = static_cast<uint8_t>(call[i] << 1);
    }
    uint8_t ssid_byte = static_cast<uint8_t>(0x60 | ((ssid & 0x0F) << 1));
    if (last) ssid_byte |= 1;
    out[6] = ssid_byte;
    return out;
}

uint16_t compute_fcs(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            if (crc & 1u) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0x8408u);
            } else {
                crc = static_cast<uint16_t>(crc >> 1);
            }
        }
    }
    return static_cast<uint16_t>(crc ^ 0xFFFF);
}

std::vector<bool> build_bits(const std::string& info,
                             const std::string& destination_call = "APRS") {
    std::vector<uint8_t> frame;
    const std::vector<uint8_t> destination =
        make_address(destination_call, 0, false);
    const std::vector<uint8_t> source = make_address("N0CALL", 9, false);
    const std::vector<uint8_t> path = make_address("WIDE2", 1, true);
    frame.insert(frame.end(), destination.begin(), destination.end());
    frame.insert(frame.end(), source.begin(), source.end());
    frame.insert(frame.end(), path.begin(), path.end());
    frame.push_back(0x03);  // UI
    frame.push_back(0xF0);  // no layer 3
    frame.insert(frame.end(), info.begin(), info.end());
    const uint16_t fcs = compute_fcs(frame);
    frame.push_back(static_cast<uint8_t>(fcs & 0xFF));
    frame.push_back(static_cast<uint8_t>(fcs >> 8));

    std::vector<bool> bits;
    const auto add_flag = [&bits]() {
        for (int i = 0; i < 8; ++i) bits.push_back(((0x7E >> i) & 1) != 0);
    };
    add_flag();
    add_flag();

    int ones = 0;
    for (uint8_t byte : frame) {
        for (int i = 0; i < 8; ++i) {
            const bool bit = ((byte >> i) & 1) != 0;
            bits.push_back(bit);
            if (bit) {
                if (++ones == 5) {
                    bits.push_back(false);
                    ones = 0;
                }
            } else {
                ones = 0;
            }
        }
    }
    add_flag();
    add_flag();
    return bits;
}

std::vector<uint8_t> modulate(const std::vector<bool>& bits) {
    constexpr int kSamplesPerBit = 40;
    constexpr int kUpsample = 25;
    constexpr double kSampleRate = 1200000.0;
    constexpr double kDeviation = 3000.0;

    std::vector<uint8_t> cu8;
    cu8.reserve(bits.size() * kSamplesPerBit * kUpsample * 2);

    bool level = false;
    double audio_phase = 0.0;
    double carrier_phase = 0.0;
    for (bool bit : bits) {
        if (!bit) level = !level;
        const double tone = level ? 1200.0 : 2200.0;
        const double audio_step = 2.0 * kPi * tone / kSampleRate;
        for (int i = 0; i < kSamplesPerBit * kUpsample; ++i) {
            const double audio = std::cos(audio_phase);
            audio_phase += audio_step;
            carrier_phase += 2.0 * kPi * kDeviation * audio / kSampleRate;
            const double i_value = std::cos(carrier_phase);
            const double q_value = std::sin(carrier_phase);
            cu8.push_back(static_cast<uint8_t>(
                std::lround((i_value * 0.75 + 1.0) * 127.5)));
            cu8.push_back(static_cast<uint8_t>(
                std::lround((q_value * 0.75 + 1.0) * 127.5)));
        }
    }
    return cu8;
}

}  // namespace

aprs::Packet decode_one(const std::string& info, bool* ok_out = nullptr,
                        const std::string& destination = "APRS") {
    aprs::Decoder decoder;
    aprs::Decoder::Config config;
    bool ok = decoder.init(config);
    std::vector<aprs::Packet> packets;
    decoder.setPacketCallback([&packets](const aprs::Packet& packet) {
        packets.push_back(packet);
    });
    if (ok) {
        const std::vector<uint8_t> iq = modulate(build_bits(info, destination));
        constexpr size_t kChunk = 65536;
        for (size_t offset = 0; offset < iq.size() / 2; offset += kChunk) {
            const size_t count = std::min(kChunk, iq.size() / 2 - offset);
            decoder.process(iq.data() + offset * 2, count);
        }
    }
    ok = ok && packets.size() == 1;
    if (ok_out != nullptr) *ok_out = ok;
    return ok ? packets[0] : aprs::Packet{};
}

int main() {
    bool ok = true;

    // Uncompressed position with course, speed and altitude.
    bool case_ok = true;
    aprs::Packet packet = decode_one(
        "!4903.50N/07201.75W-Test 088/036/A=000123", &case_ok);
    ok = ok && case_ok;
    ok = ok && packet.source == "N0CALL-9";
    ok = ok && packet.destination == "APRS";
    ok = ok && packet.has_position;
    ok = ok && std::fabs(packet.latitude - 49.058333) < 0.0002;
    ok = ok && std::fabs(packet.longitude + 72.029167) < 0.0002;
    ok = ok && std::fabs(packet.speed_knots - 36.0f) < 0.5f;
    ok = ok && std::fabs(packet.course - 88.0f) < 0.5f;
    ok = ok && packet.altitude_ft == 123;
    ok = ok && packet.symbol_table == '/';
    ok = ok && packet.symbol == '-';
    ok = ok && packet.comment.find("Test") == 0;
    std::printf("uncompressed: %.5f %.5f spd %.0f crs %.0f alt %d\n",
                packet.latitude, packet.longitude, packet.speed_knots,
                packet.course, packet.altitude_ft);

    // Compressed position, course/speed.
    packet = decode_one("!/5`=k<;>x>7P TEST", &case_ok);
    ok = ok && case_ok;
    ok = ok && packet.has_position;
    ok = ok && std::fabs(packet.latitude - 49.058333) < 0.01;
    ok = ok && std::fabs(packet.longitude + 72.029167) < 0.01;
    ok = ok && std::fabs(packet.speed_knots - 36.0f) < 1.0f;
    ok = ok && std::fabs(packet.course - 88.0f) < 1.0f;
    std::printf("compressed c/s: %.5f %.5f spd %.0f crs %.0f\n",
                packet.latitude, packet.longitude, packet.speed_knots,
                packet.course);

    // Compressed position with GGA altitude.
    packet = decode_one("!/5`=k<;>x>;KG", &case_ok);
    ok = ok && case_ok;
    ok = ok && packet.has_position;
    ok = ok && std::abs(packet.altitude_ft - 123) <= 3;
    std::printf("compressed alt: %.5f %.5f alt %d\n", packet.latitude,
                packet.longitude, packet.altitude_ft);

    // Receive-only message.
    packet = decode_one(":N0CALL-9 :Hello world{12", &case_ok);
    ok = ok && case_ok;
    ok = ok && packet.message;
    ok = ok && packet.addressee == "N0CALL-9";
    ok = ok && packet.message_text == "Hello world";
    std::printf("message: %s -> %s\n", packet.source.c_str(),
                packet.message_text.c_str());

    // Positionless weather.
    packet = decode_one(
        "_10090556c220s004g005t077r010p000P000h50b09900", &case_ok);
    ok = ok && case_ok;
    ok = ok && packet.weather;
    ok = ok && std::fabs(packet.wind_dir - 220.0f) < 0.5f;
    ok = ok && std::fabs(packet.wind_speed_mph - 4.0f) < 0.5f;
    ok = ok && std::fabs(packet.temperature_c - 25.0f) < 0.5f;
    ok = ok && std::fabs(packet.rain_hour_mm - 2.54f) < 0.05f;
    std::printf("weather: wind %.0f/%.0f temp %.1fC rain %.2fmm\n",
                packet.wind_dir, packet.wind_speed_mph,
                packet.temperature_c, packet.rain_hour_mm);

    // Mic-E position: spec example S32UVT with a +100 degree offset.
    packet = decode_one("`(_fn\"Oj/", &case_ok, "S32UVT");
    ok = ok && case_ok;
    ok = ok && packet.has_position;
    ok = ok && std::fabs(packet.latitude - 33.427333) < 0.001;
    ok = ok && std::fabs(packet.longitude + 112.129000) < 0.001;
    ok = ok && std::fabs(packet.speed_knots - 20.0f) < 0.5f;
    ok = ok && std::fabs(packet.course - 251.0f) < 0.5f;
    ok = ok && packet.symbol == 'j';
    ok = ok && packet.symbol_table == '/';
    std::printf("mic-e: %.5f %.5f spd %.0f crs %.0f sym %c%c\n",
                packet.latitude, packet.longitude, packet.speed_knots,
                packet.course, packet.symbol_table, packet.symbol);

    if (!ok) {
        std::fprintf(stderr, "APRS decoder test FAILED\n");
        return 1;
    }
    std::printf("APRS decoder test OK\n");
    return 0;
}
