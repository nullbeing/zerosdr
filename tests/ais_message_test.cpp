#include "ais/ais_message.h"

#include <cstdio>
#include <cstring>

namespace {

// Fill a 120-bit AIS name field with 20 six-bit characters. Characters in
// 32..63 keep their value; letters are mapped back to 1..26 etc.
void makeNameField(uint8_t out[15], const char* text) {
    std::memset(out, 0, 15);
    int bit = 0;
    const int length = static_cast<int>(std::strlen(text));
    for (int i = 0; i < 20; ++i) {
        int code = 0;
        if (i < length) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            code = (c >= 64) ? c - 64 : c;
        }
        for (int b = 5; b >= 0; --b) {
            if ((code >> b) & 1) {
                out[bit >> 3] |= static_cast<uint8_t>(1u << (7 - (bit & 7)));
            }
            ++bit;
        }
    }
}

}  // namespace

int main() {
    uint8_t field[15];

    // AIS pads with '@'. Some transponders append vendor bytes instead of
    // stopping at the first '@'; the name must end there.
    makeNameField(field, "SUISHUIBA52@A1=?A1=?");
    const std::string name = ais::sixBitString(field, 0, 120);
    if (name != "SUISHUIBA52") {
        std::printf("FAIL: got '%s'\n", name.c_str());
        return 1;
    }

    makeNameField(field, "ABC@@@@@@@@@@@@@@@@@");
    const std::string padded = ais::sixBitString(field, 0, 120);
    if (padded != "ABC") {
        std::printf("FAIL: padded got '%s'\n", padded.c_str());
        return 1;
    }

    std::printf("ais_message_test: ok\n");
    return 0;
}
