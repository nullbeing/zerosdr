// Scanner DSP regression test (development helper, not installed).
#include "fft_processor.h"
#include "scanner/scanner_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace scanner;

static int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 1024000.0;

struct Tone {
    double offset_hz;
    double amplitude;
    double am_depth = 0.0;   // AM modulation depth (1 kHz)
    double fm_dev_hz = 0.0;  // FM deviation (1 kHz)
};

std::vector<uint8_t> makeIq(size_t samples, const std::vector<Tone>& tones,
                            double noise, double dc_i, unsigned seed,
                            size_t start = 0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, noise);
    std::vector<uint8_t> iq(samples * 2);
    for (size_t n = 0; n < samples; ++n) {
        const double t = static_cast<double>(n + start) / kFs;
        double re = dc_i + gauss(rng);
        double im = gauss(rng);
        for (const Tone& tone : tones) {
            double amp = tone.amplitude;
            if (tone.am_depth > 0.0) {
                amp *= 1.0 + tone.am_depth * std::sin(2 * kPi * 1000.0 * t);
            }
            double phase = 2 * kPi * tone.offset_hz * t;
            if (tone.fm_dev_hz > 0.0) {
                phase += (tone.fm_dev_hz / 1000.0) * std::sin(2 * kPi * 1000.0 * t);
            }
            re += amp * std::cos(phase);
            im += amp * std::sin(phase);
        }
        iq[2 * n] = static_cast<uint8_t>(std::clamp(127.5 + re * 128.0, 0.0, 255.0));
        iq[2 * n + 1] = static_cast<uint8_t>(std::clamp(127.5 + im * 128.0, 0.0, 255.0));
    }
    return iq;
}

double goertzel(const std::vector<int16_t>& x, double freq, double fs) {
    const double w = 2 * kPi * freq / fs;
    const double c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int16_t v : x) {
        const double s = v + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return std::sqrt(std::max(p, 0.0)) * 2.0 / x.size();
}

double rms(const std::vector<int16_t>& x) {
    double s = 0;
    for (int16_t v : x) s += double(v) * v;
    return x.empty() ? 0 : std::sqrt(s / x.size());
}

std::vector<int16_t> runChannelizer(Channelizer& ch, const std::vector<Tone>& tones,
                                    double noise, int chunks) {
    std::vector<int16_t> all;
    std::vector<int16_t> pcm;
    const size_t chunk = 51200;
    for (int c = 0; c < chunks; ++c) {
        auto iq = makeIq(chunk, tones, noise, 0.02, 7 + c, c * chunk);
        ch.process(iq.data(), iq.size(), pcm);
        // Skip the first chunk while the AGC settles.
        if (c > 0) all.insert(all.end(), pcm.begin(), pcm.end());
    }
    return all;
}

SliceGeometry geometryFor(double lo_hz, size_t fft) {
    SliceGeometry geo;
    const size_t usable = static_cast<size_t>(fft * 0.78);
    geo.centre_bin = fft / 2;
    geo.first_bin = geo.centre_bin - usable / 2;
    geo.last_bin = geo.first_bin + usable;
    geo.centre_hz = lo_hz;
    geo.bin_hz = kFs / fft;
    return geo;
}

std::vector<float> averagedSpectrum(const std::vector<Tone>& tones, double noise,
                                    double dc_i, unsigned seed) {
    FFTProcessor fft(1024);
    SpectrumAverager avg;
    std::vector<float> mag;
    for (int f = 0; f < 8; ++f) {
        auto iq = makeIq(1024, tones, noise, dc_i, seed + f, f * 1024);
        fft.process(iq, mag);
        avg.add(mag);
    }
    std::vector<float> db;
    avg.result(db);
    return db;
}

void testDetection() {
    const double lo = 125000000.0;
    const auto geo = geometryFor(lo, 1024);

    // Noise plus a strong DC offset only: no hits.
    for (unsigned seed = 1; seed < 20; ++seed) {
        const auto db = averagedSpectrum({}, 0.02, 0.05, seed * 100);
        const auto r = detectSignals(db, geo, 10.0f, 5000);
        if (!r.hits.empty()) {
            std::printf("noise seed %u gave %zu hits, first %u snr %.1f\n", seed,
                        r.hits.size(), r.hits[0].hz, r.hits[0].snr_db);
        }
        CHECK(r.hits.empty());
    }

    // Two carriers: +120.7 kHz (snaps to 125.120 MHz) and -251 kHz.
    const auto db = averagedSpectrum({{120700.0, 0.02}, {-251000.0, 0.01}}, 0.02,
                                     0.05, 999);
    const auto r = detectSignals(db, geo, 10.0f, 5000);
    CHECK(r.hits.size() == 2);
    bool found_a = false, found_b = false;
    for (const auto& h : r.hits) {
        if (h.hz == 125120000u) found_a = true;
        if (h.hz == 124750000u) found_b = true;
        CHECK(h.snr_db > 10.0f);
    }
    CHECK(found_a);
    CHECK(found_b);

    // Range clipping drops the lower one.
    auto clipped = geo;
    clipped.min_hz = 125000000u;
    CHECK(detectSignals(db, clipped, 10.0f, 5000).hits.size() == 1);

    // Channel level at the +120.7k carrier is well above the floor.
    const float level = channelPeakDb(db, geo, 120700.0, 5000.0);
    CHECK(level - r.floor_db > 20.0f);
}

void testAm() {
    Channelizer ch;
    ch.setMode(Mode::AM);
    ch.setOffset(200000.0);
    // Wanted AM at +200 kHz, a 10 dB stronger carrier one 8.33 kHz channel
    // away and a far stronger one 100 kHz away.
    const auto pcm = runChannelizer(
        ch, {{200000.0, 0.05, 0.8}, {208333.0, 0.15}, {300000.0, 0.4}}, 0.01, 6);
    CHECK(!pcm.empty());
    const double tone = goertzel(pcm, 1000.0, 48000.0);
    const double total = rms(pcm) * std::sqrt(2.0);
    std::printf("AM: 1 kHz %.0f of %.0f (%.0f%%), %zu samples\n", tone, total,
                100.0 * tone / total, pcm.size());
    CHECK(tone > 3000.0);
    CHECK(tone > 0.7 * total);
}

void testNfm() {
    Channelizer ch;
    ch.setMode(Mode::NFM);
    ch.setOffset(200000.0);
    const auto pcm = runChannelizer(
        ch, {{200000.0, 0.05, 0.0, 3000.0}, {225000.0, 0.2}}, 0.01, 6);
    const double tone = goertzel(pcm, 1000.0, 48000.0);
    const double total = rms(pcm) * std::sqrt(2.0);
    std::printf("NFM: 1 kHz %.0f of %.0f (%.0f%%)\n", tone, total, 100.0 * tone / total);
    CHECK(tone > 3000.0);
    CHECK(tone > 0.7 * total);

    // Audio length tracks the input: 6 x 50 ms -> ~5 x 2400 samples kept.
    CHECK(pcm.size() > 11900 && pcm.size() < 12100);
}

void testTable() {
    SignalTable table;
    table.setTolerance(5000);
    const uint32_t a = table.observe({124950000u, -60.f, 20.f}, 10.0);
    const uint32_t a2 = table.observe({124955000u, -60.f, 22.f}, 10.5);
    CHECK(a == a2);
    const uint32_t b = table.observe({120600000u, -70.f, 12.f}, 10.6);
    CHECK(b != a);
    CHECK(table.signals().size() == 2);

    // Burst duration and activity bits.
    table.observe({124950000u, -60.f, 20.f}, 12.8);
    const TrackedSignal* s = table.find(124950000u);
    CHECK(s && std::fabs(s->burstDuration() - 2.8) < 1e-6);
    CHECK(s->heardInSecond(10) && s->heardInSecond(12) && !s->heardInSecond(11));
    table.observe({124950000u, -60.f, 20.f}, 20.0);  // gap > 2.5 s: new burst
    s = table.find(124950000u);
    CHECK(s->burstDuration() == 0.0);
    CHECK(s->heardInSecond(10) && s->heardInSecond(20));

    // pickNext: strongest fresh, honouring exclude / lockout / snooze.
    table.observe({120600000u, -70.f, 12.f}, 19.5);
    CHECK(table.pickNext(19.0, 20.0, 0) == 124950000u);
    CHECK(table.pickNext(19.0, 20.0, 124950000u) == 120600000u);
    CHECK(table.toggleLockout(124950000u));
    CHECK(table.pickNext(19.0, 20.0, 0) == 120600000u);
    CHECK(!table.toggleLockout(124952000u));  // near match unlocks it
    table.snooze(124950000u, 30.0);
    CHECK(table.pickNext(19.0, 20.0, 0) == 120600000u);
    CHECK(table.pickNext(19.0, 31.0, 0) == 124950000u);
    CHECK(table.pickNext(19.0, 20.0, 0, 121000000u, 130000000u) == 0);

    table.expire(40.0, 15.0);
    CHECK(table.signals().empty());
}

void testCrystalHarmonics() {
    CHECK(scanner::isCrystalHarmonic(144000000));
    CHECK(scanner::isCrystalHarmonic(432000000));
    CHECK(scanner::isCrystalHarmonic(432001000));
    CHECK(!scanner::isCrystalHarmonic(144005000));   // next 5 kHz channel
    CHECK(!scanner::isCrystalHarmonic(431993750));   // PMR-style 6.25 kHz step
    CHECK(!scanner::isCrystalHarmonic(125120000));
    CHECK(!scanner::isCrystalHarmonic(0));
}

}  // namespace

int main() {
    testCrystalHarmonics();
    testDetection();
    testAm();
    testNfm();
    testTable();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("scanner_dsp_test: all checks passed\n");
    return 0;
}
