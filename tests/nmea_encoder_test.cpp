#include "ais/nmea_encoder.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    // Type 5 frame captured on AIS1; the two expected sentences are the
    // reference output from AIS-catcher for the same frame.
    const char* bits =
        "0010100001000110011001011001010100100000000000000000000000000000"
        "1100000000110000000011001100001100110000000011000001010011001001"
        "0110000100100000100001110000011101000001000100000000010001000001"
        "0001000000000100010000010001000000000100001111000100000001010001"
        "1100000000010100011101100110110100010011010010101000000010101011"
        "0010000010000111000001110001011011000001101010110100000100010000"
        "00000100010000010001000000000100010000010101101111011001";

    ais::AisFrame frame;
    frame.bit_length = 440;
    frame.channel = 0;
    for (int i = 0; i < frame.bit_length; ++i) {
        if (bits[i] == '1') {
            frame.bits[static_cast<size_t>(i / 8)] |=
                static_cast<uint8_t>(1u << (7 - (i & 7)));
        }
    }

    ais::NmeaEncoder encoder;
    const auto sentences = encoder.encode(frame);
    const std::vector<std::string> expected = {
        "!AIVDM,2,1,0,A,56:Vb@@000033333330`TpH4pN2222222222220t0``3:6rnj581m@CQ,0*23\r\n",
        "!AIVDM,2,2,0,A,p6R3mH888888888,2*2E\r\n",
    };

    if (sentences != expected) {
        std::fprintf(stderr, "NMEA encoder mismatch\n");
        for (const auto& sentence : sentences) std::fprintf(stderr, "%s", sentence.c_str());
        return 1;
    }

    std::printf("NMEA encoder test passed\n");
    return 0;
}
