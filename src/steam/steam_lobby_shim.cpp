#include "steam/steam_lobby_shim.h"

#include "common/memory.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ghost::steam::shim {
namespace {

// ISteamMatchmaking009 methods replaced by the shim, and their vtable slots.
enum MatchmakingMethod : size_t {
    kJoinLobby, kLeaveLobby, kGetNumLobbyMembers, kGetLobbyMemberByIndex,
    kGetLobbyData, kGetLobbyMemberData, kSetLobbyMemberData, kGetLobbyOwner, kMatchmakingMethods,
};
constexpr size_t kMatchmakingSlots[kMatchmakingMethods] = {14, 15, 17, 18, 19, 24, 25, 35};
// ISteamNetworking006 vtable slots 0-7.
enum NetworkingSlot : size_t {
    kSendP2PPacket, kIsP2PPacketAvailable, kReadP2PPacket, kAcceptP2PSession,
    kCloseP2PSession, kCloseP2PChannel, kGetP2PSessionState, kAllowP2PPacketRelay, kNetworkingSlots,
};

constexpr size_t kMaxQueuedPackets = 128;

std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_localSteamId{0};
PointerHook g_matchmaking[kMatchmakingMethods];
PointerHook g_networking[kNetworkingSlots];

std::mutex g_dataMutex;
metadata::Fields g_lobbyData;        // the host's lobby data
metadata::Fields g_localMemberData;  // what the game sets for the local member

// P2P relay to the room tunnel. Used from the game's network thread.
std::mutex g_relayMutex;
SOCKET g_relaySocket = INVALID_SOCKET;
std::unique_ptr<net::WinsockSession> g_relayWinsock;
sockaddr_in g_relayAddress = {};
std::deque<std::vector<uint8_t>> g_relayQueue;

bool IsShimLobby(uint64_t lobby) { return lobby == kLobbyId && g_active.load(std::memory_order_acquire); }
bool IsShimPeer(uint64_t remote) { return g_active.load(std::memory_order_acquire) && remote == kHostMemberId; }

void* SteamInterface(const char* flatAccessor, const char* version) {
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (steam == nullptr) return nullptr;
    using Accessor = void*(__cdecl*)();
    if (const auto accessor = reinterpret_cast<Accessor>(GetProcAddress(steam, flatAccessor))) {
        if (void* result = accessor()) return result;
    }
    using GetUser = uint32_t(__cdecl*)();
    using FindInterface = void*(__cdecl*)(uint32_t, const char*);
    const auto user = reinterpret_cast<GetUser>(GetProcAddress(steam, "SteamAPI_GetHSteamUser"));
    const auto find = reinterpret_cast<FindInterface>(GetProcAddress(steam, "SteamInternal_FindOrCreateUserInterface"));
    return user && find ? find(user(), version) : nullptr;
}

bool ReadLocalSteamId() {
    void* user = SteamInterface("SteamAPI_SteamUser_v023", "SteamUser023");
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    using GetSteamId = uint64_t(__cdecl*)(void*);
    const auto getId = steam ? reinterpret_cast<GetSteamId>(GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID")) : nullptr;
    const uint64_t id = user && getId ? getId(user) : 0;
    if (id == 0 || id == kHostMemberId) return false;
    g_localSteamId.store(id, std::memory_order_release);
    return true;
}

// Called with g_relayMutex held: drains the relay socket into the queue.
void PumpRelay() {
    if (g_relaySocket == INVALID_SOCKET) return;
    for (int i = 0; i < 128; ++i) {
        uint8_t buffer[65535];
        sockaddr_in from = {};
        int length = sizeof(from);
        const int received = recvfrom(g_relaySocket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&from), &length);
        if (received == SOCKET_ERROR) break;
        if (received == 0 || !net::SameEndpoint(from, g_relayAddress) ||
            metadata::IsControl({buffer, static_cast<size_t>(received)})) continue;
        if (g_relayQueue.size() >= kMaxQueuedPackets) g_relayQueue.pop_front();
        g_relayQueue.emplace_back(buffer, buffer + received);
    }
}

void CloseRelay() {
    std::lock_guard lock(g_relayMutex);
    if (g_relaySocket != INVALID_SOCKET) closesocket(g_relaySocket);
    g_relaySocket = INVALID_SOCKET;
    g_relayQueue.clear();
    g_relayWinsock.reset();
}

// --- ISteamNetworking006 -------------------------------------------------------

bool __fastcall SendP2PPacket(void* self, uint64_t remote, const void* bytes, uint32_t size, int type, int channel) {
    if (!IsShimPeer(remote))
        return g_networking[kSendP2PPacket].Original<decltype(&SendP2PPacket)>()(self, remote, bytes, size, type, channel);
    std::lock_guard lock(g_relayMutex);
    if (g_relaySocket == INVALID_SOCKET || size > 65507) return false;
    const int sent = sendto(g_relaySocket, static_cast<const char*>(bytes), static_cast<int>(size), 0,
        reinterpret_cast<const sockaddr*>(&g_relayAddress), sizeof(g_relayAddress));
    return sent == static_cast<int>(size);
}

bool __fastcall IsP2PPacketAvailable(void* self, uint32_t* size, int channel) {
    if (!g_active.load(std::memory_order_acquire))
        return g_networking[kIsP2PPacketAvailable].Original<decltype(&IsP2PPacketAvailable)>()(self, size, channel);
    std::lock_guard lock(g_relayMutex);
    PumpRelay();
    if (g_relayQueue.empty()) return false;
    if (size) *size = static_cast<uint32_t>(g_relayQueue.front().size());
    return true;
}

bool __fastcall ReadP2PPacket(void* self, void* buffer, uint32_t capacity, uint32_t* size, uint64_t* remote, int channel) {
    if (!g_active.load(std::memory_order_acquire))
        return g_networking[kReadP2PPacket].Original<decltype(&ReadP2PPacket)>()(self, buffer, capacity, size, remote, channel);
    std::lock_guard lock(g_relayMutex);
    PumpRelay();
    if (g_relayQueue.empty() || g_relayQueue.front().size() > capacity || buffer == nullptr) return false;
    const auto& packet = g_relayQueue.front();
    std::memcpy(buffer, packet.data(), packet.size());
    if (size) *size = static_cast<uint32_t>(packet.size());
    if (remote) *remote = kHostMemberId;
    g_relayQueue.pop_front();
    return true;
}

bool __fastcall AcceptP2PSession(void* self, uint64_t remote) {
    if (IsShimPeer(remote)) return true;
    return g_networking[kAcceptP2PSession].Original<decltype(&AcceptP2PSession)>()(self, remote);
}

bool __fastcall CloseP2PSession(void* self, uint64_t remote) {
    if (IsShimPeer(remote)) {
        std::lock_guard lock(g_relayMutex);
        g_relayQueue.clear();
        return true;
    }
    return g_networking[kCloseP2PSession].Original<decltype(&CloseP2PSession)>()(self, remote);
}

bool __fastcall CloseP2PChannel(void* self, uint64_t remote, int channel) {
    if (IsShimPeer(remote)) return true;
    return g_networking[kCloseP2PChannel].Original<decltype(&CloseP2PChannel)>()(self, remote, channel);
}

bool __fastcall GetP2PSessionState(void* self, uint64_t remote, void* state) {
    if (IsShimPeer(remote)) {
        if (state) {
            std::memset(state, 0, 24);
            static_cast<uint8_t*>(state)[0] = 1;   // m_bConnectionActive
        }
        return true;
    }
    return g_networking[kGetP2PSessionState].Original<decltype(&GetP2PSessionState)>()(self, remote, state);
}

bool __fastcall AllowP2PPacketRelay(void* self, bool allow) {
    if (g_active.load()) return true;
    return g_networking[kAllowP2PPacketRelay].Original<decltype(&AllowP2PPacketRelay)>()(self, allow);
}

// --- ISteamMatchmaking009 ------------------------------------------------------

PointerHook& Matchmaking(MatchmakingMethod method) { return g_matchmaking[method]; }

uint64_t __fastcall JoinLobby(void* self, uint64_t lobby) {
    if (IsShimLobby(lobby)) return 1;
    return Matchmaking(kJoinLobby).Original<decltype(&JoinLobby)>()(self, lobby);
}

void __fastcall LeaveLobby(void* self, uint64_t lobby) {
    if (IsShimLobby(lobby)) return;
    Matchmaking(kLeaveLobby).Original<decltype(&LeaveLobby)>()(self, lobby);
}

int __fastcall GetNumLobbyMembers(void* self, uint64_t lobby) {
    if (IsShimLobby(lobby)) return 2;
    return Matchmaking(kGetNumLobbyMembers).Original<decltype(&GetNumLobbyMembers)>()(self, lobby);
}

uint64_t* __fastcall GetLobbyMemberByIndex(void* self, uint64_t* out, uint64_t lobby, int index) {
    if (IsShimLobby(lobby)) {
        if (out) *out = index == 0 ? kHostMemberId : index == 1 ? g_localSteamId.load() : 0;
        return out;
    }
    return Matchmaking(kGetLobbyMemberByIndex).Original<decltype(&GetLobbyMemberByIndex)>()(self, out, lobby, index);
}

const char* __fastcall GetLobbyData(void* self, uint64_t lobby, const char* key) {
    if (!IsShimLobby(lobby)) return Matchmaking(kGetLobbyData).Original<decltype(&GetLobbyData)>()(self, lobby, key);
    thread_local std::string value;
    std::lock_guard lock(g_dataMutex);
    const std::string_view query = key ? key : "";
    // The game reads the host's version as gamever_<host member id>.
    const auto found = g_lobbyData.find(query == "gamever_1" ? "HostGameVersion" : query);
    value = found != g_lobbyData.end() ? found->second : "";
    return value.c_str();
}

const char* __fastcall GetLobbyMemberData(void* self, uint64_t lobby, uint64_t member, const char* key) {
    if (!IsShimLobby(lobby))
        return Matchmaking(kGetLobbyMemberData).Original<decltype(&GetLobbyMemberData)>()(self, lobby, member, key);
    if (member == kHostMemberId) {
        const std::string_view query = key ? key : "";
        return GetLobbyData(self, lobby, query == "address" ? "HostAddress" : key);
    }
    thread_local std::string value;
    std::lock_guard lock(g_dataMutex);
    const auto found = g_localMemberData.find(key ? key : "");
    value = member == g_localSteamId.load() && found != g_localMemberData.end() ? found->second : "";
    return value.c_str();
}

void __fastcall SetLobbyMemberData(void* self, uint64_t lobby, const char* key, const char* value) {
    if (!IsShimLobby(lobby)) {
        Matchmaking(kSetLobbyMemberData).Original<decltype(&SetLobbyMemberData)>()(self, lobby, key, value);
        return;
    }
    if (!key || !value || std::strlen(key) > metadata::kMaxKey || std::strlen(value) > metadata::kMaxValue) return;
    std::lock_guard lock(g_dataMutex);
    g_localMemberData[key] = value;
}

uint64_t* __fastcall GetLobbyOwner(void* self, uint64_t* out, uint64_t lobby) {
    if (IsShimLobby(lobby)) {
        if (out) *out = kHostMemberId;
        return out;
    }
    return Matchmaking(kGetLobbyOwner).Original<decltype(&GetLobbyOwner)>()(self, out, lobby);
}

} // namespace

bool Install() {
    void* matchmaking = SteamInterface("SteamAPI_SteamMatchmaking_v009", "SteamMatchMaking009");
    void* networking = SteamInterface("SteamAPI_SteamNetworking_v006", "SteamNetworking006");
    if (!matchmaking || !networking || !ReadLocalSteamId()) return false;
    auto** matchmakingTable = *reinterpret_cast<void***>(matchmaking);
    auto** networkingTable = *reinterpret_cast<void***>(networking);

    void* const matchmakingDetours[kMatchmakingMethods] = {
        reinterpret_cast<void*>(&JoinLobby), reinterpret_cast<void*>(&LeaveLobby),
        reinterpret_cast<void*>(&GetNumLobbyMembers), reinterpret_cast<void*>(&GetLobbyMemberByIndex),
        reinterpret_cast<void*>(&GetLobbyData), reinterpret_cast<void*>(&GetLobbyMemberData),
        reinterpret_cast<void*>(&SetLobbyMemberData), reinterpret_cast<void*>(&GetLobbyOwner)};
    void* const networkingDetours[kNetworkingSlots] = {
        reinterpret_cast<void*>(&SendP2PPacket), reinterpret_cast<void*>(&IsP2PPacketAvailable),
        reinterpret_cast<void*>(&ReadP2PPacket), reinterpret_cast<void*>(&AcceptP2PSession),
        reinterpret_cast<void*>(&CloseP2PSession), reinterpret_cast<void*>(&CloseP2PChannel),
        reinterpret_cast<void*>(&GetP2PSessionState), reinterpret_cast<void*>(&AllowP2PPacketRelay)};

    for (size_t i = 0; i < kMatchmakingMethods; ++i) {
        if (!g_matchmaking[i].Install(matchmakingTable + kMatchmakingSlots[i], matchmakingDetours[i]) ||
            !g_networking[i].Install(networkingTable + i, networkingDetours[i])) {
            Uninstall();
            return false;
        }
    }
    return true;
}

void Uninstall() {
    for (auto& hook : g_matchmaking) hook.Restore();
    for (auto& hook : g_networking) hook.Restore();
}

bool OpenRelay(const sockaddr_in& tunnelEndpoint) {
    CloseRelay();
    std::lock_guard lock(g_relayMutex);
    g_relayWinsock = std::make_unique<net::WinsockSession>();
    if (!g_relayWinsock->Started()) return false;
    g_relayAddress = tunnelEndpoint;
    g_relaySocket = net::OpenNonblockingUdpSocket();
    return g_relaySocket != INVALID_SOCKET;
}

void Activate(metadata::Fields hostLobbyData) {
    {
        std::lock_guard lock(g_dataMutex);
        g_lobbyData = std::move(hostLobbyData);
        g_localMemberData.clear();
    }
    g_active.store(true, std::memory_order_release);
}

void Deactivate() {
    g_active.store(false, std::memory_order_release);
    CloseRelay();
    std::lock_guard lock(g_dataMutex);
    g_lobbyData.clear();
    g_localMemberData.clear();
}

bool Active() { return g_active.load(std::memory_order_acquire); }

} // namespace ghost::steam::shim
