#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ghost::metadata {

// Control messages have their own socket on the guest and never enter RakNet.
inline constexpr std::array<uint8_t, 16> kMagic = {
    'G', 'H', 'O', 'S', 'T', '-', 'L', 'O', 'B', 'B', 'Y', '-', 0x91, 0x27, 0x01, 0xd3
};
inline constexpr size_t kMaxPacket = 4096;
inline constexpr size_t kMaxFields = 24;
inline constexpr size_t kMaxKey = 96;
inline constexpr size_t kMaxValue = 512;
using Fields = std::map<std::string, std::string, std::less<>>;

struct Message {
    bool response = false;
    uint64_t nonce = 0;
    uint64_t generation = 0;
    Fields fields;
};

inline bool IsControl(std::span<const uint8_t> data) {
    return data.size() >= kMagic.size() &&
        std::memcmp(data.data(), kMagic.data(), kMagic.size()) == 0;
}

inline void Append(std::vector<uint8_t>& data, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) data.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

inline std::vector<uint8_t> Encode(const Message& message) {
    if (message.nonce == 0 || message.fields.size() > kMaxFields ||
        (!message.response && (message.generation != 0 || !message.fields.empty()))) return {};
    std::vector<uint8_t> data(kMagic.begin(), kMagic.end());
    data.push_back(message.response ? 2 : 1);
    Append(data, message.nonce, 8);
    if (!message.response) return data;
    Append(data, message.generation, 8);
    Append(data, message.fields.size(), 1);
    for (const auto& [key, value] : message.fields) {
        if (key.empty() || key.size() > kMaxKey || value.size() > kMaxValue ||
            key.find('\0') != std::string::npos || value.find('\0') != std::string::npos) return {};
        Append(data, key.size(), 1);
        Append(data, value.size(), 2);
        data.insert(data.end(), key.begin(), key.end());
        data.insert(data.end(), value.begin(), value.end());
        if (data.size() > kMaxPacket) return {};
    }
    return data;
}

// Decode transactionally: malformed or truncated responses never replace a snapshot.
inline bool Decode(std::span<const uint8_t> data, Message& result) {
    if (!IsControl(data) || data.size() < 25 || data.size() > kMaxPacket) return false;
    Message message;
    if (data[16] != 1 && data[16] != 2) return false;
    message.response = data[16] == 2;
    size_t offset = 17;
    const auto take = [&](size_t bytes) {
        uint64_t value = 0;
        for (size_t i = 0; i < bytes; ++i) value |= uint64_t(data[offset++]) << (8 * i);
        return value;
    };
    message.nonce = take(8);
    if (message.nonce == 0) return false;
    if (message.response) {
        if (data.size() < 34) return false;
        message.generation = take(8);
        const size_t count = take(1);
        if (count > kMaxFields) return false;
        for (size_t i = 0; i < count; ++i) {
            if (data.size() - offset < 3) return false;
            const size_t keySize = take(1);
            const size_t valueSize = take(2);
            if (keySize == 0 || keySize > kMaxKey || valueSize > kMaxValue ||
                data.size() - offset < keySize + valueSize) return false;
            std::string key(reinterpret_cast<const char*>(data.data() + offset), keySize);
            offset += keySize;
            std::string value(reinterpret_cast<const char*>(data.data() + offset), valueSize);
            offset += valueSize;
            if (key.find('\0') != std::string::npos || value.find('\0') != std::string::npos ||
                !message.fields.emplace(std::move(key), std::move(value)).second) return false;
        }
    }
    if (offset != data.size()) return false;
    result = std::move(message);
    return true;
}

inline std::string Missing(const Fields& fields) {
    std::string missing;
    for (const auto key : {"PresetId", "gameMode", "HostPublicKey", "HostGameVersion"}) {
        const auto found = fields.find(key);
        if (found == fields.end() || found->second.empty()) {
            if (!missing.empty()) missing += ", ";
            missing += key;
        }
    }
    const auto contract = fields.find("ContractId");
    const auto match = fields.find("matchId");
    // Native PopulateSession registers to an existing match when ContractId is
    // empty. Preserve that path instead of requiring a prematurely chosen map.
    if ((contract == fields.end() || contract->second.empty()) &&
        (match == fields.end() || match->second.empty() ||
            match->second == "00000000-0000-0000-0000-000000000000")) {
        if (!missing.empty()) missing += ", ";
        missing += "ContractId or matchId";
    }
    return missing;
}

} // namespace ghost::metadata
