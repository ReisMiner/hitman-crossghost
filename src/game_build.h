#pragma once

// Identifies the running HITMAN executable. Every offset in this project is
// only valid for these exact HITMAN 3.290.1.0 builds.

#include <string>
#include <string_view>

namespace ghost {

inline constexpr std::string_view kEpicSha256 =
    "bde3d362efeaf546587d0f3d282bcc83990741295359c90f5ec8668fa51aa00f";
inline constexpr std::string_view kSteamSha256 =
    "0e0dc96305bf26c2d9b1d3e65c31f549987a779901fa2750d2f93cb0d4a441ef";

inline constexpr char kGameVersion[] = "3.290.1.0";

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
