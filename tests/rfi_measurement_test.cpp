#include "pages/rfi_measurement.h"
#include "fft_processor.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
void check(bool pass) { if (!pass) throw std::runtime_error("RFI measurement assertion failed"); }
int main() {
    RfiReference ref;
    ref.begin();
    for (unsigned i = 0; i < 7; ++i) ref.add({1,1,1,1,1});
    check(!ref.ready());
    ref.add({1,1,1,1,1});
    check(ref.ready());
    check(std::fabs(ref.medianChange({0.1f,0.1f,0.1f,0.1f,0.1f}) + 10) < 0.001f);
    check(ref.medianChange({1,1,100,1,1}) == 0);
    check(ref.medianChange({1,1,1,1,1}) == 0);
    ref.reset(); check(!ref.ready());
    ref.begin(); ref.add({1,1}); ref.add({1}); check(!ref.collecting() && !ref.ready());
    ref.begin(); ref.add({}); check(ref.count() == 0);
    // Unequal sweeps must average power, not dB.
    for (unsigned i = 0; i < 8; ++i) ref.add({i < 4 ? 1.0f : 0.01f});
    check(std::fabs(ref.power()[0] - 0.505f) < 0.0001f);
    FFTProcessor fft(1024); fft.setIQImbalanceCorrection(false);
    auto tone = [&](float amplitude) {
        std::vector<uint8_t> iq(2048);
        for (size_t i = 0; i < 1024; ++i) {
            const double phase = 2 * 3.141592653589793 * 100 * i / 1024;
            iq[2*i] = static_cast<uint8_t>(std::lround(127 + amplitude * std::cos(phase)));
            iq[2*i+1] = static_cast<uint8_t>(std::lround(127 + amplitude * std::sin(phase)));
        }
        std::vector<float> mag; fft.process(iq, mag);
        return RfiReference::fftPower(mag[612], 1024);
    };
    check(std::fabs(RfiReference::db(tone(64)) - RfiReference::db(tone(32)) - 6.0206f) < 0.1f);
    std::cout << "RFI measurement tests passed\n";
}
