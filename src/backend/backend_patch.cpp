#include <Windows.h>
#include <winhttp.h>

#include "backend/backend_patch.h"
#include "common/memory.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifndef GHOST_BACKEND_URL
#define GHOST_BACKEND_URL ""
#endif

namespace ghost {
namespace {

// Patch sites (RVAs) in each HITMAN 3.280 build, matching LocalGhost's patcher.
struct BackendLayout {
    uintptr_t domain;    // ZConfigString with the backend host name (256-byte buffer)
    uintptr_t auth1;     // jne skipping the custom-backend authentication path
    uintptr_t auth2;     // je  skipping the custom-backend authentication path
    uintptr_t pin;       // jne enforcing IOI's certificate pin
    uintptr_t format;    // "https://{0}" URL format string
    uintptr_t length;    // length of that format string
    uintptr_t dynamic;   // "use dynamic resources" flag
};

constexpr BackendLayout kEpicLayout{0x2342a48, 0xcac96c, 0xcac9a2, 0xfde03d, 0x1eb51c0, 0xcd2cbd, 0x3bed138};
constexpr BackendLayout kSteamLayout{0x234e608, 0xcac67c, 0xcac6b2, 0xfe060d, 0x1ebfa20, 0xcd2a8d, 0x3bfa538};

using Bytes = std::vector<uint8_t>;

Bytes CString(const char* text) { return {text, text + std::strlen(text) + 1}; }

struct ParsedOrigin {
    std::string authority;   // host[:port]
    bool https = false;
};

ParsedOrigin ParseOrigin(const std::string& url) {
    if (!std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; }))
        throw std::runtime_error("Backend origin must be ASCII without whitespace");
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) || !parts.dwHostNameLength ||
        parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
        (parts.dwUrlPathLength && std::wstring_view(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/"))
        throw std::runtime_error("Invalid backend origin");

    const std::wstring hostname(parts.lpszHostName, parts.dwHostNameLength);
    ParsedOrigin origin;
    origin.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const bool loopback = hostname == L"localhost" || hostname == L"127.0.0.1";
    if (!origin.https && !(parts.nScheme == INTERNET_SCHEME_HTTP && loopback))
        throw std::runtime_error("Public backend requires HTTPS");
    for (wchar_t c : hostname) origin.authority.push_back(static_cast<char>(c));
    if (parts.nPort != (origin.https ? 443 : 80)) origin.authority += ":" + std::to_string(parts.nPort);
    if (origin.authority.size() >= 256) throw std::runtime_error("Backend domain exceeds native buffer");
    return origin;
}

} // namespace

BackendPatch::~BackendPatch() { Restore(); }

std::string BackendPatch::Status() const {
    std::lock_guard lock(m_mutex);
    return m_status;
}

void BackendPatch::Restore() {
    for (auto it = m_patches.rbegin(); it != m_patches.rend(); ++it) {
        if (IsReadable(it->address, it->installed.size()) &&
            !std::memcmp(it->address, it->installed.data(), it->installed.size()))
            WriteCode(it->address, it->original.data(), it->original.size());
    }
    m_patches.clear();
}

bool BackendPatch::Apply(Storefront store) {
    std::lock_guard lock(m_mutex);
    if (!m_patches.empty()) return true;
    const std::string url = GHOST_BACKEND_URL;
    if (url.empty()) {
        m_status = "Backend origin is not provisioned in this development build";
        return false;
    }
    try {
        if (store == Storefront::Unsupported) throw std::runtime_error("Unsupported backend executable");
        const ParsedOrigin origin = ParseOrigin(url);
        const BackendLayout& layout = store == Storefront::Epic ? kEpicLayout : kSteamLayout;

        // Build and verify the whole plan before the first write.
        std::vector<Patch> plan;
        const auto add = [&](uintptr_t rva, Bytes installed, std::vector<Bytes> accepted) {
            auto* address = GameAddress<uint8_t>(rva);
            if (!IsReadable(address, installed.size())) throw std::runtime_error("Backend patch address unavailable");
            const bool matches = accepted.empty() || std::any_of(accepted.begin(), accepted.end(), [&](const Bytes& bytes) {
                return bytes.size() == installed.size() && !std::memcmp(address, bytes.data(), bytes.size());
            });
            if (!matches) throw std::runtime_error("Backend patch signature mismatch");
            plan.push_back({address, {address, address + installed.size()}, std::move(installed)});
        };

        const auto* domain = GameAddress<const char>(layout.domain);
        if (!IsReadable(domain, 256) || !domain[0] || !std::memchr(domain, 0, 256))
            throw std::runtime_error("Native backend configuration is not initialized");

        // Each entry accepts the original bytes or our already-patched bytes.
        add(layout.auth1, {0x90, 0x90}, {{0x75, 0x08}, {0x90, 0x90}});
        add(layout.auth2, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90},
            {{0x0f, 0x84, 0xc2, 0, 0, 0}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}});
        // Remove only IOI-specific pinning for the custom HTTPS origin. The
        // game's ordinary certificate-chain verification remains in place.
        if (origin.https) add(layout.pin, {0x90, 0xe9}, {{0x0f, 0x85}, {0x90, 0xe9}});
        Bytes httpFormat = CString("http://{0}");
        httpFormat.resize(12);
        add(layout.format, origin.https ? CString("https://{0}") : httpFormat, {CString("https://{0}"), httpFormat});
        add(layout.length, {static_cast<uint8_t>(origin.https ? 11 : 10)}, {{11}, {10}});
        add(layout.dynamic, {0}, {{0}, {1}});
        add(layout.domain, CString(origin.authority.c_str()), {});

        // Record each patch before writing it so a failure part-way through
        // restores everything already written.
        for (auto& patch : plan) {
            m_patches.push_back(std::move(patch));
            const Patch& added = m_patches.back();
            if (!WriteCode(added.address, added.installed.data(), added.installed.size()))
                throw std::runtime_error("Backend memory update failed");
        }
        m_status = "Automatic backend: " + url;
        return true;
    } catch (const std::exception& error) {
        Restore();
        m_status = error.what();
        return false;
    }
}

} // namespace ghost
