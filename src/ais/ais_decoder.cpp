#include "ais/ais_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <vector>

namespace ais {
namespace {

constexpr double kPi = 3.14159265358979323846;

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

inline void set_bit(std::array<uint8_t, 160>& buffer, int index, bool value) {
    const int byte = index >> 3;
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (index & 7)));
    if (value) {
        buffer[static_cast<size_t>(byte)] |= mask;
    } else {
        buffer[static_cast<size_t>(byte)] &= static_cast<uint8_t>(~mask);
    }
}

inline bool get_bit(const std::array<uint8_t, 160>& buffer, int index) {
    return (buffer[static_cast<size_t>(index >> 3)] >> (7 - (index & 7))) & 1;
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

class CicDecimator {
public:
    void init(int decimation, int stages = 5) {
        decimation_ = decimation;
        stages_ = stages;
        count_ = 0;
        integrators_.fill(std::complex<float>(0.0f, 0.0f));
        combs_.fill(std::complex<float>(0.0f, 0.0f));
        gain_ = 1.0f;
    }

    bool process(const std::complex<float>& sample, std::complex<float>& out) {
        std::complex<float> value = sample;
        for (int i = 0; i < stages_; ++i) {
            integrators_[static_cast<size_t>(i)] += value;
            value = integrators_[static_cast<size_t>(i)];
        }

        if (++count_ < decimation_) return false;
        count_ = 0;

        std::complex<float> combined = value;
        for (int i = 0; i < stages_; ++i) {
            const std::complex<float> previous = combs_[static_cast<size_t>(i)];
            combs_[static_cast<size_t>(i)] = combined;
            combined -= previous;
        }
        out = combined * gain_;
        return true;
    }

private:
    int decimation_ = 1;
    int stages_ = 5;
    int count_ = 0;
    std::array<std::complex<float>, 5> integrators_{};
    std::array<std::complex<float>, 5> combs_{};
    float gain_ = 1.0f;
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

class Nco {
public:
    void init(double sample_rate, double frequency) {
        const double delta = 2.0 * kPi * frequency / sample_rate;
        rotation_ = std::complex<float>(static_cast<float>(std::cos(delta)),
                                        static_cast<float>(std::sin(delta)));
        oscillator_ = std::complex<float>(1.0f, 0.0f);
        count_ = 0;
    }

    std::complex<float> mix(const std::complex<float>& sample) {
        const std::complex<float> mixed = sample * oscillator_;
        oscillator_ *= rotation_;
        if (++count_ >= 4096) {
            count_ = 0;
            const float magnitude = std::abs(oscillator_);
            if (magnitude > 1e-6f) oscillator_ /= magnitude;
        }
        return mixed;
    }

private:
    std::complex<float> oscillator_{1.0f, 0.0f};
    std::complex<float> rotation_{1.0f, 0.0f};
    int count_ = 0;
};

class HdlcFramer {
public:
    bool push(bool bit, AisFrame& frame) {
        switch (state_) {
            case State::Training:
                if (bit != last_bit_) {
                    ++position_;
                } else {
                    if (position_ > 4) {
                        state_ = State::StartFlag;
                        position_ = bit ? 3 : 1;
                    } else {
                        state_ = State::Training;
                        position_ = 0;
                    }
                }
                break;

            case State::StartFlag:
                if (position_ == 7) {
                    if (!bit) {
                        state_ = State::Data;
                        position_ = 0;
                        ones_ = 0;
                        message_.fill(0);
                        ++flags;
                    } else {
                        state_ = State::Training;
                        position_ = 0;
                    }
                } else if (bit) {
                    ++position_;
                } else {
                    state_ = State::Training;
                    position_ = 0;
                }
                break;

            case State::Data: {
                if (position_ < kMaxBits) set_bit(message_, position_, bit);
                ++position_;

                if (bit) {
                    if (ones_ == 5) {
                        const int length = position_ - 7;
                        bool decoded = false;
                        bool valid = (length >= 32 && length <= kMaxBits && crc_ok(length));
                        if (!valid && length >= 32 && length <= kMaxBits) {
                            valid = correct_single_bit(length);
                        }
                        if (valid) {
                            frame.bits.fill(0);
                            std::memcpy(frame.bits.data(), message_.data(),
                                        static_cast<size_t>((length + 7) / 8));
                            frame.bit_length = length;
                            decoded = true;
                        } else {
                            ++crc_fail;
                        }
                        state_ = State::Training;
                        position_ = 0;
                        ones_ = 0;
                        last_bit_ = bit;
                        return decoded;
                    }
                    ++ones_;
                } else {
                    if (ones_ == 5) --position_;  // remove the stuffed zero
                    ones_ = 0;
                }

                if (position_ >= kMaxBits) {
                    ++aborts;
                    state_ = State::Training;
                    position_ = 0;
                    ones_ = 0;
                }
                break;
            }
        }

        last_bit_ = bit;
        return false;
    }

    uint64_t flags = 0;
    uint64_t crc_fail = 0;
    uint64_t aborts = 0;

private:
    enum class State { Training, StartFlag, Data };

    static uint8_t reverse_byte(uint8_t value) {
        uint8_t reversed = 0;
        for (int b = 0; b < 8; ++b) {
            reversed = static_cast<uint8_t>((reversed << 1) | (value & 1u));
            value >>= 1;
        }
        return reversed;
    }

    int field_type() const {
        return static_cast<int>(reverse_byte(message_[0]) >> 2);
    }

    static bool known_length(int type, int data_bits) {
        switch (type) {
            case 1: case 2: case 3: case 4: case 9: case 11: case 18:
                return data_bits == 168;
            case 5:  return data_bits == 424;
            case 10: return data_bits == 72;
            case 19: return data_bits == 312;
            case 24: return data_bits == 160 || data_bits == 168;
            case 27: return data_bits == 96;
            case 21: return data_bits >= 271;
            default: return false;
        }
    }

    bool correct_single_bit(int length) {
        const int data_bits = length - 16;
        const int type = field_type();
        if (!known_length(type, data_bits)) return false;

        for (int i = 0; i < length; ++i) {
            set_bit(message_, i, !get_bit(message_, i));
            const bool ok = crc_ok(length);
            if (ok) return true;
            set_bit(message_, i, !get_bit(message_, i));
        }
        return false;
    }

    bool crc_ok(int length) const {
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < length; ++i) {
            const uint16_t bit = get_bit(message_, i) ? 1u : 0u;
            if ((crc ^ bit) & 1u) {
                crc = static_cast<uint16_t>((crc >> 1) ^ 0x8408u);
            } else {
                crc = static_cast<uint16_t>(crc >> 1);
            }
        }
        return crc == 0xF0B8;
    }

    static constexpr int kMaxBits = 140 * 8;

    State state_ = State::Training;
    bool last_bit_ = false;
    int position_ = 0;
    int ones_ = 0;
    std::array<uint8_t, 160> message_{};
};

class PhaseSampler {
public:
    bool push(float value, AisFrame& frame) {
        const bool level = value > 0.0f;
        if (!have_previous_) {
            previous_level_ = level;
            have_previous_ = true;
            return false;
        }

        // NRZI: a transition is a 0, no transition is a 1.
        const bool bit = (level == previous_level_) ? 1 : 0;
        previous_level_ = level;
        return framer_.push(bit, frame);
    }

    uint64_t flags() const { return framer_.flags; }
    uint64_t crc_fail() const { return framer_.crc_fail; }
    uint64_t aborts() const { return framer_.aborts; }

private:
    bool have_previous_ = false;
    bool previous_level_ = false;
    HdlcFramer framer_;
};

class Channel {
public:
    void init(int index, double nco_frequency, double sample_rate,
              const std::vector<float>& stage1_taps, int stage1_decimation,
              const std::vector<float>& stage2_taps, int stage2_decimation,
              const std::vector<float>& discriminator_taps) {
        index_ = index;
        nco_.init(sample_rate, nco_frequency);
        stage1_.init(stage1_taps, stage1_decimation);
        stage2_.init(stage2_taps, stage2_decimation);
        discriminator_.init(discriminator_taps);
        previous_iq_ = std::complex<float>(0.0f, 0.0f);
        have_previous_ = false;
        dc_input_ = 0.0f;
        dc_output_ = 0.0f;
        agc_ = 0.0f;
        sample_phase_ = 0;
    }

    bool process(const std::complex<float>& input, double timestamp,
                 AisFrame& frame) {
        const std::complex<float> mixed = nco_.mix(input);

        std::complex<float> stage1_out;
        if (!stage1_.process(mixed, stage1_out)) return false;

        std::complex<float> stage2_out;
        if (!stage2_.process(stage1_out, stage2_out)) return false;

        if (!have_previous_) {
            previous_iq_ = stage2_out;
            have_previous_ = true;
            return false;
        }

        const std::complex<float> product = stage2_out * std::conj(previous_iq_);
        previous_iq_ = stage2_out;

        const float discriminator =
            std::atan2(product.imag(), product.real()) / static_cast<float>(kPi);

        // First-order DC blocker. It removes the constant frequency offset
        // without touching the 9600-baud modulation.
        const float dc = discriminator - dc_input_ + 0.999f * dc_output_;
        dc_input_ = discriminator;
        dc_output_ = dc;

        const float filtered = discriminator_.process(dc);

        agc_ = 0.999f * agc_ + 0.001f * std::fabs(filtered);
        const float normalized = filtered / std::max(agc_, 1e-4f);

        const int phase = sample_phase_;
        sample_phase_ = (sample_phase_ + 1) % 5;

        if (!samplers_[static_cast<size_t>(phase)].push(normalized, frame)) {
            return false;
        }

        frame.channel = static_cast<uint8_t>(index_);
        frame.phase = static_cast<uint8_t>(phase);
        frame.timestamp = timestamp;
        frame.signal_db = 20.0f * std::log10(agc_ + 1e-9f);
        return true;
    }

    uint64_t flags() const {
        uint64_t total = 0;
        for (const auto& sampler : samplers_) total += sampler.flags();
        return total;
    }

    uint64_t crc_fail() const {
        uint64_t total = 0;
        for (const auto& sampler : samplers_) total += sampler.crc_fail();
        return total;
    }

    uint64_t aborts() const {
        uint64_t total = 0;
        for (const auto& sampler : samplers_) total += sampler.aborts();
        return total;
    }

private:
    int index_ = 0;
    Nco nco_;
    ComplexDecimator stage1_;
    ComplexDecimator stage2_;
    Fir discriminator_;
    std::complex<float> previous_iq_{0.0f, 0.0f};
    bool have_previous_ = false;
    float dc_input_ = 0.0f;
    float dc_output_ = 0.0f;
    float agc_ = 0.0f;
    int sample_phase_ = 0;
    std::array<PhaseSampler, 5> samplers_;
};

struct RecentFrame {
    uint64_t hash = 0;
    uint8_t channel = 0;
    double timestamp = 0.0;
};

}  // namespace

struct AisDecoder::Impl {
    Config config;
    FrameCallback callback;
    std::vector<Channel> channels;
    std::deque<RecentFrame> recent;
    uint64_t total_samples = 0;
    uint64_t frames = 0;
    uint64_t duplicates = 0;

    void on_frame(const AisFrame& frame) {
        uint64_t hash = 1469598103934665603ULL;
        const int bytes = (frame.bit_length + 7) / 8;
        for (int i = 0; i < bytes; ++i) {
            hash ^= frame.bits[static_cast<size_t>(i)];
            hash *= 1099511628211ULL;
        }
        hash ^= static_cast<uint64_t>(frame.bit_length) * 0x9E3779B97F4A7C15ULL;
        hash ^= static_cast<uint64_t>(frame.channel + 1) * 0xC2B2AE3D27D4EB4FULL;

        for (const RecentFrame& entry : recent) {
            if (entry.hash == hash && entry.channel == frame.channel &&
                frame.timestamp - entry.timestamp < 0.75) {
                ++duplicates;
                return;
            }
        }

        recent.push_back({hash, frame.channel, frame.timestamp});
        while (recent.size() > 64 ||
               (!recent.empty() &&
                frame.timestamp - recent.front().timestamp > 2.0)) {
            recent.pop_front();
        }

        ++frames;
        if (callback) callback(frame);
    }
};

AisDecoder::AisDecoder() : impl_(std::make_unique<Impl>()) {}

AisDecoder::~AisDecoder() = default;

bool AisDecoder::init(const Config& config) {
    // The five-phase sampler relies on exactly five samples per bit. Keep the
    // native decoder on the clean 1.92 Msps rate (1.92M / 40 = 48k).
    if (std::fabs(config.sample_rate - 1920000.0) > 1.0) return false;

    impl_->config = config;
    impl_->channels.clear();
    impl_->recent.clear();
    impl_->total_samples = 0;
    impl_->frames = 0;
    impl_->duplicates = 0;

    const std::vector<float> stage1_taps =
        design_lowpass(config.sample_rate, 60000.0, 31);
    const std::vector<float> stage2_taps =
        design_lowpass(240000.0, config.stage2_cutoff_hz, 47);
    const std::vector<float> discriminator_taps =
        design_lowpass(48000.0, config.discriminator_cutoff_hz, 31);

    if (config.channel_a) {
        impl_->channels.emplace_back();
        impl_->channels.back().init(
            0, config.channel_offset_hz, config.sample_rate,
            stage1_taps, 8, stage2_taps, 5, discriminator_taps);
    }
    if (config.channel_b) {
        impl_->channels.emplace_back();
        impl_->channels.back().init(
            1, -config.channel_offset_hz, config.sample_rate,
            stage1_taps, 8, stage2_taps, 5, discriminator_taps);
    }

    return !impl_->channels.empty();
}

void AisDecoder::reset() {
    const FrameCallback callback = impl_->callback;
    Config config = impl_->config;
    init(config);
    impl_->callback = callback;
}

void AisDecoder::process(const uint8_t* cu8, size_t sample_count) {
    if (cu8 == nullptr || sample_count == 0) return;

    constexpr float kScale = 1.0f / 127.5f;
    for (size_t n = 0; n < sample_count; ++n) {
        const float i =
            (static_cast<float>(cu8[2 * n]) - 127.5f) * kScale;
        const float q =
            (static_cast<float>(cu8[2 * n + 1]) - 127.5f) * kScale;
        const std::complex<float> sample(i, q);
        const double timestamp =
            static_cast<double>(impl_->total_samples) / impl_->config.sample_rate;

        for (Channel& channel : impl_->channels) {
            AisFrame frame;
            if (channel.process(sample, timestamp, frame)) {
                impl_->on_frame(frame);
            }
        }
        ++impl_->total_samples;
    }
}

void AisDecoder::setFrameCallback(FrameCallback callback) {
    impl_->callback = std::move(callback);
}

AisDecoder::Stats AisDecoder::stats() const {
    Stats result;
    result.samples = impl_->total_samples;
    result.frames = impl_->frames;
    result.duplicates = impl_->duplicates;
    for (const Channel& channel : impl_->channels) {
        result.flags += channel.flags();
        result.crc_fail += channel.crc_fail();
        result.aborts += channel.aborts();
    }
    return result;
}

const AisDecoder::Config& AisDecoder::config() const {
    return impl_->config;
}

}  // namespace ais
