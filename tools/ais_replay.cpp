#include "ais/ais_decoder.h"
#include "ais/ais_message.h"
#include "ais/nmea_encoder.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct VesselSummary {
    std::string name;
    int type = 0;
    int messages = 0;
    bool has_position = false;
    double latitude = 0.0;
    double longitude = 0.0;
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program
        << " <file.cu8> [--rate 1920000] [--ppm N] [--fc2 Hz] [--fcd Hz]"
        << " [--nmea]\n"
        << "\n"
        << "Reads interleaved 8-bit unsigned IQ (RTL-SDR CU8) and prints every\n"
        << "AIS frame that passes the HDLC CRC. The file must be centred between\n"
        << "AIS1 161.975 MHz and AIS2 162.025 MHz.\n";
}

const char* typeName(int type) {
    switch (type) {
        case 1: return "position A";
        case 2: return "position A";
        case 3: return "position A";
        case 4: return "base station";
        case 5: return "static/voyage";
        case 18: return "position B";
        case 19: return "extended B";
        case 21: return "AtoN";
        case 24: return "static B";
        case 27: return "long range";
        default: return "other";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string path;
    double sample_rate = 1920000.0;
    double ppm = 0.0;
    double fc2 = 7000.0;
    double fcd = 6800.0;
    bool nmea_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rate" && i + 1 < argc) {
            sample_rate = std::stod(argv[++i]);
        } else if (arg == "--ppm" && i + 1 < argc) {
            ppm = std::stod(argv[++i]);
        } else if (arg == "--fc2" && i + 1 < argc) {
            fc2 = std::stod(argv[++i]);
        } else if (arg == "--fcd" && i + 1 < argc) {
            fcd = std::stod(argv[++i]);
        } else if (arg == "--nmea") {
            nmea_only = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (!arg.empty() && arg[0] != '-') {
            path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (path.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Cannot open " << path << "\n";
        return 1;
    }

    ais::AisDecoder::Config config;
    config.sample_rate = sample_rate;
    config.ppm = ppm;
    config.stage2_cutoff_hz = fc2;
    config.discriminator_cutoff_hz = fcd;

    ais::AisDecoder decoder;
    if (!decoder.init(config)) {
        std::cerr << "Unsupported sample rate " << sample_rate
                  << " (the native decoder currently expects 1920000)\n";
        return 1;
    }

    std::map<uint32_t, VesselSummary> vessels;
    uint64_t printed = 0;
    ais::NmeaEncoder encoder;

    decoder.setFrameCallback([&](const ais::AisFrame& frame) {
        if (nmea_only) {
            const auto sentences = encoder.encode(frame);
            for (const auto& sentence : sentences) std::cout << sentence;
            ++printed;
            return;
        }

        ais::AisMessage message;
        if (!ais::parseAisMessage(frame, message) || !message.valid) return;

        ++printed;
        VesselSummary& summary = vessels[message.mmsi];
        summary.messages++;
        summary.type = message.type;
        if (!message.name.empty()) summary.name = message.name;
        if (message.has_position) {
            summary.has_position = true;
            summary.latitude = message.latitude;
            summary.longitude = message.longitude;
        }

        std::cout << std::fixed << std::setprecision(2) << std::setw(8)
                  << frame.timestamp << "  "
                  << static_cast<char>('A' + frame.channel) << " p"
                  << static_cast<int>(frame.phase) << "  t"
                  << std::setw(2) << message.type << "  mmsi="
                  << std::setw(9) << message.mmsi;

        if (message.has_position) {
            std::cout << "  " << std::setprecision(6) << message.latitude
                      << "," << message.longitude << std::setprecision(2);
        }
        if (message.sog >= 0.0f) std::cout << "  sog=" << message.sog;
        if (message.cog >= 0.0f) std::cout << "  cog=" << message.cog;
        if (message.heading >= 0) std::cout << "  hdg=" << message.heading;
        if (!message.name.empty()) std::cout << "  \"" << message.name << "\"";
        if (!message.callsign.empty()) std::cout << "  [" << message.callsign << "]";
        if (!message.destination.empty()) std::cout << "  ->" << message.destination;
        std::cout << "  " << typeName(message.type);
        std::cout << "\n";
    });

    constexpr size_t kChunkSamples = 1u << 17;  // 131072 IQ pairs
    std::vector<uint8_t> buffer(kChunkSamples * 2);

    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes = input.gcount();
        if (bytes <= 0) break;
        decoder.process(buffer.data(), static_cast<size_t>(bytes) / 2);
    }

    const ais::AisDecoder::Stats stats = decoder.stats();
    std::cout << "\n---\n";
    std::cout << "frames: " << stats.frames
              << "  unique MMSI: " << vessels.size()
              << "  duplicates: " << stats.duplicates << "\n";
    std::cout << "flags: " << stats.flags
              << "  crc_fail: " << stats.crc_fail
              << "  aborts: " << stats.aborts << "\n";

    std::cout << "\nvessels:\n";
    for (const auto& entry : vessels) {
        const VesselSummary& vessel = entry.second;
        std::cout << "  " << std::setw(9) << entry.first << "  "
                  << std::setw(3) << vessel.messages << " msg  t"
                  << std::setw(2) << vessel.type;
        if (vessel.has_position) {
            std::cout << "  " << std::setprecision(6) << vessel.latitude << ","
                      << vessel.longitude << std::setprecision(2);
        }
        if (!vessel.name.empty()) std::cout << "  \"" << vessel.name << "\"";
        std::cout << "\n";
    }

    return 0;
}
