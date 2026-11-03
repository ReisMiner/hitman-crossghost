#pragma once

// Identifies the running HITMAN executable. Every offset in this project is
// only valid for these exact HITMAN 3.290.1.0 builds (2026-11-03 patch; same
// size, PE timestamp and addresses as the 2026-10-30 builds, only the hash changed).

#include <string>
#include <string_view>

namespace ghost {

inline constexpr std::string_view kEpicSha256 =
    "a63b678253965d037b497a53858a74cc2196184df5661c5385fc9bf9fc45ee82";
inline constexpr std::string_view kSteamSha256 =
    "0a337c7979a48b1c2d2e4cf7e80a63bf634c527b6bd301dfe46db8bb7f977b52";

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
