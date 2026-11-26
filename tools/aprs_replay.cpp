#include "aprs/aprs_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <cu8 file> [sample rate]\n", argv[0]);
        return 1;
    }

    aprs::Decoder decoder;
    aprs::Decoder::Config config;
    if (argc >= 3) config.sample_rate = std::atof(argv[2]);
    if (!decoder.init(config)) {
        std::fprintf(stderr, "APRS replay needs 1.2 Msps\n");
        return 1;
    }

    const bool show_hex = std::getenv("APRS_HEX") != nullptr;
    unsigned long long packets = 0;
    decoder.setPacketCallback([&packets, show_hex](const aprs::Packet& packet) {
        ++packets;
        if (show_hex) {
            std::fprintf(stderr, "%s dest=%s info=", packet.source.c_str(),
                         packet.destination.c_str());
            for (unsigned char value : packet.info) {
                std::fprintf(stderr, "%02x ", value);
            }
            std::fprintf(stderr, "\n");
        }
        std::printf("%-9s %c ", packet.source.c_str(),
                    packet.type != 0 ? packet.type : '?');
        if (packet.has_position) {
            std::printf("%.5f,%.5f", packet.latitude, packet.longitude);
            if (packet.speed_knots >= 0.0f) {
                std::printf(" %.0fkn", packet.speed_knots);
            }
            if (packet.course >= 0.0f) {
                std::printf(" %03.0f", packet.course);
            }
            if (packet.altitude_ft > -1000) {
                std::printf(" %dft", packet.altitude_ft);
            }
        } else if (packet.weather) {
            std::printf("WX %.0f/%.0f %.1fC", packet.wind_dir,
                        packet.wind_speed_mph, packet.temperature_c);
        } else if (packet.message) {
            std::printf("MSG %s", packet.message_text.c_str());
        }
        std::printf(" | %s\n", packet.info.c_str());
        std::fflush(stdout);
    });

    FILE* file = std::fopen(argv[1], "rb");
    if (file == nullptr) {
        std::perror(argv[1]);
        return 1;
    }

    std::vector<uint8_t> buffer(65536);
    size_t read = 0;
    while ((read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
        decoder.process(buffer.data(), read / 2);
    }
    std::fclose(file);

    const aprs::Decoder::Stats stats = decoder.stats();
    std::printf("packets=%llu crc_fail=%llu duplicates=%llu bits=%llu "
                "transitions=%llu samples=%llu\n",
                static_cast<unsigned long long>(stats.packets),
                static_cast<unsigned long long>(stats.crc_fail),
                static_cast<unsigned long long>(stats.duplicates),
                static_cast<unsigned long long>(stats.bits),
                static_cast<unsigned long long>(stats.transitions),
                static_cast<unsigned long long>(stats.samples));
    return packets > 0 ? 0 : 2;
}
