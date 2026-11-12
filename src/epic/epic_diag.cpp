#include "epic/epic_diag.h"

#include "common/diag_log.h"
#include "common/memory.h"

#include <cstring>
#include <format>
#include <string>

namespace ghost::epic {
namespace {

// Epic 3.290.1 addresses (RVAs), used read-only for diagnostics.
// Network object: +0x08 RakPeer, +0x11 role (1 = host), +0x13 session active,
// +0x28 identity source used by ZNetPlayer's "is local" check.
constexpr uintptr_t kNetworkObject = 0x3187eb0;
// ZPlayerRegistry (static): 4 slots of 0xd0 bytes at +0x50; slot +0x00 id,
// slot +0x50 ZNetPlayer (+0x48 name, +0x58 owner RakNet GUID).
constexpr uintptr_t kPlayerRegistry = 0x3151300;
// Multiplayer manager: +0xf8 ready count, +0x12c four {player id, value} pairs.
constexpr uintptr_t kMultiplayerManager = 0x314deb8;
// Multiplayer context: +0x20 contract, +0x30 backend matchId, +0x60 players required.
constexpr uintptr_t kMultiplayerContext = 0x31517d8;
constexpr size_t kRakPeerGuid = 0x5e0;

template <typename T>
bool Read(const void* base, size_t offset, T& out) {
    if (base == nullptr) return false;
    const char* field = static_cast<const char*>(base) + offset;
    if (!IsReadable(field, sizeof(T))) return false;
    std::memcpy(&out, field, sizeof(T));
    return true;
}

void* ReadPtr(const void* base, size_t offset) {
    return base != nullptr ? ReadPointer(static_cast<const char*>(base) + offset) : nullptr;
}

std::string Text(const void* base, size_t offset) {
    std::string value;
    if (base == nullptr || !ReadGameString(static_cast<const char*>(base) + offset, value)) return "?";
    return diag::Brief(value);
}

} // namespace

void LogSessionState() {
    static uint64_t lastCheck = 0;
    static std::string lastLine;
    const uint64_t now = GetTickCount64();
    if (now - lastCheck < 1000) return;
    lastCheck = now;

    void* net = ReadPointer(GameAddress(kNetworkObject));
    uint8_t role = 0, active = 0;
    uint64_t myGuid = 0, altGuid = 0;
    Read(net, 0x11, role);
    Read(net, 0x13, active);
    Read(ReadPtr(net, 0x08), kRakPeerGuid, myGuid);
    void* identity = ReadPtr(net, 0x28);
    const bool identityOverride = ReadPtr(identity, 0x28) != nullptr;
    Read(identity, 0x50, altGuid);

    std::string line = std::format("state: net role={} active={} myGuid={:#x} idOverride={} altGuid={:#x} | slots",
        role, active, myGuid, identityOverride, altGuid);
    const char* registry = GameAddress<char>(kPlayerRegistry);
    for (int i = 0; i < 4; ++i) {
        const char* slot = registry + 0x50 + i * 0xd0;
        int id = -1;
        Read(slot, 0, id);
        if (id == -1) continue;
        void* player = ReadPtr(slot, 0x50);
        uint64_t guid = 0;
        Read(player, 0x58, guid);
        line += std::format(" [id={} {} guid={:#x} name='{}'{}]", id, player ? "bound" : "UNBOUND", guid,
            player ? Text(player, 0x48) : "", player && guid == myGuid ? " (me)" : "");
    }

    void* manager = ReadPointer(GameAddress(kMultiplayerManager));
    int readyCount = 0;
    Read(manager, 0xf8, readyCount);
    line += std::format(" | ready count={}", readyCount);
    for (int i = 0; i < 4; ++i) {
        int id = -1, value = -1;
        Read(manager, 0x12c + i * 8, id);
        Read(manager, 0x130 + i * 8, value);
        if (id != -1) line += std::format(" {}:{}", id, value);
    }

    void* context = ReadPointer(GameAddress(kMultiplayerContext));
    uint32_t required = 0;
    Read(context, 0x60, required);
    line += std::format(" | match '{}' contract '{}' required={}", Text(context, 0x30), Text(context, 0x20), required);

    if (line != lastLine) {
        diag::Write(line);
        lastLine = std::move(line);
    }
}

} // namespace ghost::epic
