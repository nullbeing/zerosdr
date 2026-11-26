#include "aprs/aprs_decoder.h"

#include <cstdio>
#include <cstdint>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <raw float32 mono 48k file>\n",
                     argv[0]);
        return 1;
    }

    aprs::Decoder decoder;
    aprs::Decoder::Config config;
    if (!decoder.init(config)) {
        std::fprintf(stderr, "APRS audio replay init failed\n");
        return 1;
    }

    unsigned long long packets = 0;
    decoder.setPacketCallback([&packets](const aprs::Packet& packet) {
        ++packets;
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
        } else if (packet.weather) {
            std::printf("WX %.0f/%.0f", packet.wind_dir,
                        packet.wind_speed_mph);
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

    std::vector<float> buffer(4096);
    size_t read = 0;
    while ((read = std::fread(buffer.data(), sizeof(float), buffer.size(),
                              file)) > 0) {
        decoder.processAudio(buffer.data(), read);
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
