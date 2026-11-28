#include "core/decimator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

// Interleaved uint8 IQ tone at `tone_hz` with the given peak amplitude.
std::vector<uint8_t> makeTone(uint32_t sample_rate, double tone_hz,
                              double amplitude, size_t samples) {
    std::vector<uint8_t> out(samples * 2);
    const double step = 2.0 * M_PI * tone_hz / sample_rate;
    for (size_t i = 0; i < samples; ++i) {
        const double i_val = amplitude * std::cos(step * i);
        const double q_val = amplitude * std::sin(step * i);
        out[i * 2] = static_cast<uint8_t>(
            std::lround(std::clamp(127.0 + i_val * 127.0, 0.0, 255.0)));
        out[i * 2 + 1] = static_cast<uint8_t>(
            std::lround(std::clamp(127.0 + q_val * 127.0, 0.0, 255.0)));
    }
    return out;
}

double rms(const std::vector<uint8_t>& iq, size_t skip_samples = 0) {
    if (iq.size() <= skip_samples * 2) return 0.0;
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = skip_samples; i < iq.size() / 2; ++i) {
        const double vi = (iq[i * 2] - 127.0) / 128.0;
        const double vq = (iq[i * 2 + 1] - 127.0) / 128.0;
        sum += vi * vi + vq * vq;
        ++count;
    }
    return std::sqrt(sum / static_cast<double>(count));
}

}  // namespace

int main() {
    // Passthrough when no decimation is requested.
    Decimator pass;
    pass.configure(1024000, 1);
    {
        const auto input = makeTone(1024000, 5000.0, 0.8, 300);
        std::vector<uint8_t> output;
        pass.process(input, output);
        check(output == input, "factor 1 is a copy");
        check(pass.taps() == 0, "factor 1 has no filter");
    }

    // 288 kHz -> 48 kHz, factor 6, 5 kHz tone: length and amplitude.
    Decimator dec;
    dec.configure(288000, 6);
    check(dec.taps() == 32 * 6, "taps scale with the factor");

    std::vector<uint8_t> output;
    {
        const auto input = makeTone(288000, 5000.0, 0.5, 28800);  // 100 ms
        dec.process(input, output);
        // The first `taps` samples are filter warm-up, so the stream starts
        // 32 outputs (192 / 6) short and then stays on the input/6 grid.
        const size_t expected = 28800 / 6 - 192 / 6;
        const size_t got = output.size() / 2;
        check(got <= expected + 2 && got + 2 >= expected,
              "output length is input / 6 minus warm-up");
        const double in_rms = rms(input, 200);
        const double out_rms = rms(output, 200);
        check(std::fabs(out_rms - in_rms) < 0.05 * in_rms,
              "passband tone keeps its amplitude");
    }

    // Streaming across chunks must produce the same total length and not
    // reset the filter at the boundary.
    {
        Decimator streamed;
        streamed.configure(288000, 6);
        const auto input = makeTone(288000, 5000.0, 0.5, 28800);
        std::vector<uint8_t> joined;
        std::vector<uint8_t> part;
        for (size_t offset = 0; offset < 28800; offset += 5760) {
            streamed.process(input.data() + offset * 2, 5760, part);
            joined.insert(joined.end(), part.begin(), part.end());
        }
        const long long diff =
            static_cast<long long>(joined.size()) -
            static_cast<long long>(output.size());
        check(diff <= 2 && diff >= -2,
              "chunked output matches one-shot length");
        const double one = rms(output, 200);
        const double many = rms(joined, 200);
        check(std::fabs(one - many) < 0.01, "chunk boundaries stay continuous");
        check(many > 0.5 * one, "chunked tone survives");
    }

    // A tone above the 24 kHz output Nyquist must be removed by the filter.
    {
        Decimator alias;
        alias.configure(288000, 6);
        const auto input = makeTone(288000, 40000.0, 0.8, 28800);
        std::vector<uint8_t> filtered;
        alias.process(input, filtered);
        const double in_rms = rms(input, 200);
        const double out_rms = rms(filtered, 200);
        check(in_rms > 0.3, "alias test tone present at the input");
        check(out_rms < 0.02 * in_rms, "out-of-band tone is rejected");
    }

    // DC offset (the uint8 midpoint) must survive with unity gain.
    {
        Decimator dc;
        dc.configure(384000, 2);
        std::vector<uint8_t> input(4800, 127);
        for (size_t i = 0; i < input.size(); i += 2) {
            input[i] = 200;
            input[i + 1] = 60;
        }
        std::vector<uint8_t> filtered;
        dc.process(input, filtered);
        check(!filtered.empty(), "dc output");
        check(filtered[filtered.size() - 2] >= 199 &&
                  filtered[filtered.size() - 2] <= 201,
              "dc i preserved");
        check(filtered[filtered.size() - 1] >= 59 &&
                  filtered[filtered.size() - 1] <= 61,
              "dc q preserved");
    }

    if (failures != 0) {
        std::printf("decimator_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("decimator_test: ok\n");
    return 0;
}
