#pragma once

// Identifies the running HITMAN executable. Every offset in this project is
// only valid for these exact HITMAN 3.280.0.0 builds.

#include <string>
#include <string_view>

namespace ghost {

inline constexpr std::string_view kEpicSha256 =
    "005412cf59daa1aadfba8deb05f097f5418aec57ddc98216483d93bf5b0119b1";
inline constexpr std::string_view kSteamSha256 =
    "5633ec37835e8ce3612fbe6b3043b16e6d6695c1ba305a1d2473c702cc31ed7b";

enum class Storefront { Unsupported, Epic, Steam };

constexpr Storefront IdentifyStorefront(std::string_view executableSha256) {
    if (executableSha256 == kEpicSha256) return Storefront::Epic;
    if (executableSha256 == kSteamSha256) return Storefront::Steam;
    return Storefront::Unsupported;
}

// SHA-256 of HITMAN3.exe in lowercase hex, or empty if it cannot be read.
// Computed once; hashing the executable takes a moment.
const std::string& GameExecutableSha256();

inline Storefront DetectStorefront() { return IdentifyStorefront(GameExecutableSha256()); }

} // namespace ghost
