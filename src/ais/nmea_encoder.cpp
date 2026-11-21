#include "ais/nmea_encoder.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace ais {
namespace {

constexpr int kMaxPayload = 56;  // matches common AIVDM two-part frames

uint8_t reverseByte(uint8_t value) {
    uint8_t reversed = 0;
    for (int i = 0; i < 8; ++i) {
        reversed = static_cast<uint8_t>((reversed << 1) | (value & 1u));
        value >>= 1;
    }
    return reversed;
}

bool fieldBit(const uint8_t* data, int index) {
    return (data[index >> 3] >> (7 - (index & 7))) & 1;
}

char armor(uint8_t value) {
    return static_cast<char>(value < 40 ? value + 48 : value + 56);
}

}  // namespace

std::vector<std::string> NmeaEncoder::encode(const AisFrame& frame) const {
    std::vector<std::string> result;
    if (frame.bit_length < 16 + 38) return result;

    std::array<uint8_t, 140> field{};
    for (size_t i = 0; i < field.size(); ++i) {
        field[i] = reverseByte(frame.bits[i]);
    }

    const int data_bits = frame.bit_length - 16;
    const int fill = (6 - data_bits % 6) % 6;
    const int characters = (data_bits + fill) / 6;

    std::string payload;
    payload.reserve(static_cast<size_t>(characters));
    for (int c = 0; c < characters; ++c) {
        uint8_t value = 0;
        for (int b = 0; b < 6; ++b) {
            const int bit = c * 6 + b;
            value = static_cast<uint8_t>((value << 1) |
                (bit < data_bits && fieldBit(field.data(), bit) ? 1u : 0u));
        }
        payload.push_back(armor(value));
    }

    const int parts = (characters + kMaxPayload - 1) / kMaxPayload;
    if (parts <= 0) return result;

    for (int part = 0; part < parts; ++part) {
        const int offset = part * kMaxPayload;
        const int count = std::min(kMaxPayload, characters - offset);
        const std::string chunk = payload.substr(static_cast<size_t>(offset),
                                                 static_cast<size_t>(count));
        const int part_fill = (part == parts - 1) ? fill : 0;
        const char channel = static_cast<char>('A' + frame.channel);

        char header[64];
        if (parts == 1) {
            std::snprintf(header, sizeof(header), "!AIVDM,1,1,,%c,",
                          channel);
        } else {
            std::snprintf(header, sizeof(header), "!AIVDM,%d,%d,0,%c,",
                          parts, part + 1, channel);
        }

        std::string sentence(header);
        sentence += chunk;
        sentence += ',';
        sentence += std::to_string(part_fill);

        uint8_t checksum = 0;
        for (size_t i = 1; i < sentence.size(); ++i) {
            checksum ^= static_cast<uint8_t>(sentence[i]);
        }

        char tail[8];
        std::snprintf(tail, sizeof(tail), "*%02X\r\n", checksum);
        sentence += tail;
        result.push_back(std::move(sentence));
    }

    return result;
}

bool NmeaEncoder::isDuplicate(const AisFrame& frame) {
    uint64_t hash = 1469598103934665603ULL;
    const int bytes = (frame.bit_length + 7) / 8;
    for (int i = 0; i < bytes; ++i) {
        hash ^= frame.bits[static_cast<size_t>(i)];
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<uint64_t>(frame.bit_length) * 0x9E3779B97F4A7C15ULL;

    for (const auto& entry : recent_) {
        if (entry.first == hash && frame.timestamp - entry.second < 0.75) {
            return true;
        }
    }

    recent_.emplace_back(hash, frame.timestamp);
    while (recent_.size() > 64 ||
           (!recent_.empty() &&
            frame.timestamp - recent_.front().second > 2.0)) {
        recent_.pop_front();
    }
    return false;
}

NmeaUdpSender::~NmeaUdpSender() {
    close();
}

bool NmeaUdpSender::open(const std::string& host, int port) {
    close();

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    int broadcast = 1;
    setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &broadcast,
               sizeof(broadcast));

    address_ = {};
    address_.sin_family = AF_INET;
    address_.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &address_.sin_addr) != 1) {
        close();
        return false;
    }
    return true;
}

void NmeaUdpSender::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool NmeaUdpSender::send(const std::string& sentence) {
    if (fd_ < 0) return false;
    const ssize_t sent = ::sendto(
        fd_, sentence.data(), sentence.size(), 0,
        reinterpret_cast<const struct sockaddr*>(&address_), sizeof(address_));
    return sent == static_cast<ssize_t>(sentence.size());
}

}  // namespace ais
