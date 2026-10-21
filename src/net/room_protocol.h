#pragma once

#include "net/lobby_metadata.h"
#include <Windows.h>
#include <bcrypt.h>
#include <array>

namespace ghost::rooms {
inline constexpr size_t kMaxPayload = 65443;
enum class Kind : uint8_t { Hello = 1, Ack = 2, Up = 3, Down = 4, Gone = 5 };
using RoomId = std::array<uint8_t, 16>;
using Token = std::array<uint8_t, 32>;

template<size_t N> bool Unhex(std::string_view text, std::array<uint8_t, N>& out) {
    if (text.size() != N * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < N; ++i) {
        const int hi = nibble(text[i * 2]), lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// One algorithm provider per tunnel; packet HMAC handles are short-lived.
class Authenticator {
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
public:
    Authenticator() { BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG); }
    ~Authenticator() { if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0); }
    Authenticator(const Authenticator&) = delete;
    bool Sign(const Token& token, std::span<const uint8_t> data, Token& digest) const {
        return m_algorithm && BCryptHash(m_algorithm, const_cast<PUCHAR>(token.data()), static_cast<ULONG>(token.size()),
            const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size())) >= 0;
    }
};
struct Packet {
    Kind kind{};
    uint8_t channel = 0;
    uint64_t sequence = 0;
    std::span<const uint8_t> payload;
};
inline std::vector<uint8_t> Encode(const Authenticator& auth, const RoomId& room, const Token& token,
    uint8_t role, Kind kind, uint8_t channel, uint64_t sequence, std::span<const uint8_t> payload = {}) {
    if (!sequence || payload.size() > kMaxPayload) return {};
    std::vector<uint8_t> out{'G','C','R','1', static_cast<uint8_t>(kind), role, channel, 0};
    out.insert(out.end(), room.begin(), room.end());
    metadata::Append(out, sequence, 8);
    out.insert(out.end(), payload.begin(), payload.end());
    Token mac;
    if (!auth.Sign(token, out, mac)) return {};
    out.insert(out.end(), mac.begin(), mac.end());
    return out;
}
inline bool Decode(const Authenticator& auth, const RoomId& room, const Token& token, uint8_t role,
    std::span<const uint8_t> data, Packet& packet) {
    if (data.size() < 64 || data.size() > 65507 || std::memcmp(data.data(), "GCR1", 4) ||
        data[5] != role || data[7] || std::memcmp(data.data() + 8, room.data(), 16)) return false;
    Token mac;
    if (!auth.Sign(token, data.first(data.size() - 32), mac)) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < mac.size(); ++i) difference |= mac[i] ^ data[data.size() - 32 + i];
    if (difference) return false;
    packet.kind = static_cast<Kind>(data[4]); packet.channel = data[6]; packet.sequence = 0;
    for (size_t i = 0; i < 8; ++i) packet.sequence |= uint64_t(data[24 + i]) << (8 * i);
    packet.payload = data.subspan(32, data.size() - 64);
    return true;
}
struct ReplayWindow {
    uint64_t high = 0, bits = 0;
    bool Accept(uint64_t sequence) {
        if (!sequence) return false;
        if (sequence > high) {
            const auto shift = sequence - high;
            bits = shift >= 64 ? 1 : (bits << shift) | 1;
            high = sequence;
            return true;
        }
        const auto delta = high - sequence;
        if (delta >= 64 || (bits & (uint64_t(1) << delta))) return false;
        bits |= uint64_t(1) << delta;
        return true;
    }
};
} // namespace ghost::rooms
