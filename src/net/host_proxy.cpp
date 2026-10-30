#include "net/host_proxy.h"
#include "net/lobby_metadata.h"
#include "net/udp.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace ghost {
namespace {

constexpr uint16_t kNativeRakNetPort = 30000;
constexpr unsigned char kRakNetOfflineMagic[16] = {
    0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78,
};

// Keys the guest's synthetic lobby needs; everything else is ignored.
constexpr std::string_view kPublishedKeys[] = {
    "ContractId", "PresetId", "gameMode", "HostAddress", "HostPublicKey", "HostName", "HostGameVersion", "matchId",
};

std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_peerGeneration{0};
std::thread g_thread;
SOCKET g_socket = INVALID_SOCKET;
bool g_wsaStarted = false;

struct LobbyMetadata {
    std::mutex mutex;
    metadata::Fields fields;
    uint64_t generation = 0;
    bool lobbyActive = false;
    bool transportReady = false;
} g_lobby;

void Forward(SOCKET socket, std::span<const uint8_t> datagram, const sockaddr_in& destination) {
    sendto(socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
}

void AnswerMetadataQuery(SOCKET socket, std::span<const uint8_t> datagram, const sockaddr_in& source) {
    metadata::Message request;
    if (!metadata::Decode(datagram, request) || request.response) return;
    metadata::Message response;
    response.response = true;
    response.nonce = request.nonce;
    {
        std::lock_guard lock(g_lobby.mutex);
        if (g_lobby.lobbyActive && g_lobby.transportReady) {
            response.generation = g_lobby.generation;
            response.fields = g_lobby.fields;
        }
    }
    const auto reply = metadata::Encode(response);
    if (!reply.empty()) Forward(socket, reply, source);
}

bool IsRakNetConnectionRequest(std::span<const uint8_t> datagram) {
    return datagram.size() >= 18 && datagram[0] == 0x05 &&
        std::memcmp(datagram.data() + 1, kRakNetOfflineMagic, sizeof(kRakNetOfflineMagic)) == 0;
}

void ProxyThread(SOCKET socket) {
    const sockaddr_in native = net::LoopbackEndpoint(kNativeRakNetPort);

    // A HITMAN guest has two UDP endpoints: native RakNet (source :30000) and
    // the game-data socket (an ephemeral port). Both must reach the same native
    // peer or the game-level session never sees the guest.
    sockaddr_in raknetGuest = {};
    sockaddr_in appGuest = {};
    bool raknetKnown = false;
    bool appKnown = false;
    uint64_t generation = g_peerGeneration.load(std::memory_order_acquire);

    while (g_running.load(std::memory_order_acquire)) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket, &readSet);
        timeval timeout = {0, 250000};
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) break;
        if (ready <= 0) continue;

        unsigned char buffer[65535];
        sockaddr_in source = {};
        int sourceLength = sizeof(source);
        const int received = recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength);
        if (received == SOCKET_ERROR || received == 0) continue;
        const std::span<const uint8_t> datagram(buffer, static_cast<size_t>(received));

        if (metadata::IsControl(datagram)) {
            AnswerMetadataQuery(socket, datagram, source);
            continue;
        }

        const uint64_t currentGeneration = g_peerGeneration.load(std::memory_order_acquire);
        if (currentGeneration != generation) {
            generation = currentGeneration;
            raknetKnown = false;
            appKnown = false;
        }

        if (net::SameEndpoint(source, native)) {
            if (raknetKnown) Forward(socket, datagram, raknetGuest);
            if (appKnown && !net::SameEndpoint(appGuest, raknetGuest)) Forward(socket, datagram, appGuest);
            continue;
        }

        if (IsRakNetConnectionRequest(datagram)) {
            raknetGuest = source;
            raknetKnown = true;
            Forward(socket, datagram, native);
            continue;
        }

        // The app socket is a different source port on the same guest machine.
        // Accept it only after the RakNet handshake so stray traffic is ignored.
        if (raknetKnown && source.sin_addr.s_addr == raknetGuest.sin_addr.s_addr) {
            appGuest = source;
            appKnown = true;
            Forward(socket, datagram, native);
        }
    }
    g_running.store(false, std::memory_order_release);
}

} // namespace

void ResetHostProxyPeer() {
    g_peerGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void BeginHostLobby() {
    std::lock_guard lock(g_lobby.mutex);
    g_lobby.fields.clear();
    ++g_lobby.generation;
    g_lobby.lobbyActive = true;
    g_lobby.transportReady = false;
}

void EndHostLobby() {
    std::lock_guard lock(g_lobby.mutex);
    g_lobby.lobbyActive = false;
    g_lobby.transportReady = false;
    g_lobby.fields.clear();
}

void SetHostLobbyTransportReady() {
    std::lock_guard lock(g_lobby.mutex);
    g_lobby.transportReady = g_lobby.lobbyActive;
}

bool IsHostLobbyReady() {
    std::lock_guard lock(g_lobby.mutex);
    return g_lobby.lobbyActive && g_lobby.transportReady && metadata::Missing(g_lobby.fields).empty();
}

std::string HostLobbyBlocker() {
    std::lock_guard lock(g_lobby.mutex);
    if (!g_lobby.lobbyActive) return "Host: waiting for a native host lobby (open Ghost Mode and choose a map)";
    if (!g_lobby.transportReady) return "Host: waiting for the lobby's UDP transport";
    const std::string missing = metadata::Missing(g_lobby.fields);
    return missing.empty() ? std::string() : "Host: waiting for lobby data: " + missing;
}

void PublishHostLobbyField(std::string_view key, std::string_view value) {
    // Native version fields are gamever_<platform member id>. The guest maps
    // this host's published version to its synthetic host member ID.
    if (key.starts_with("gamever_")) key = "HostGameVersion";
    if (std::find(std::begin(kPublishedKeys), std::end(kPublishedKeys), key) == std::end(kPublishedKeys)) return;
    if (value.size() > metadata::kMaxValue || value.find('\0') != std::string_view::npos) return;
    std::lock_guard lock(g_lobby.mutex);
    if (g_lobby.lobbyActive) g_lobby.fields[std::string(key)] = value;
}

bool StartHostProxy(uint16_t port) {
    if (g_running.load(std::memory_order_acquire)) return true;
    StopHostProxy();

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_wsaStarted = true;
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) {
        StopHostProxy();
        return false;
    }
    net::EnlargeSocketBuffers(g_socket);
    const sockaddr_in listen = net::LoopbackEndpoint(port);
    if (bind(g_socket, reinterpret_cast<const sockaddr*>(&listen), sizeof(listen)) == SOCKET_ERROR) {
        StopHostProxy();
        return false;
    }
    g_running.store(true, std::memory_order_release);
    g_thread = std::thread(ProxyThread, g_socket);
    return true;
}

void StopHostProxy() {
    g_running.store(false, std::memory_order_release);
    // select has a 250 ms timeout. Join before closing to avoid racing recvfrom
    // or closing a socket handle that Windows has already recycled.
    if (g_thread.joinable()) g_thread.join();
    if (g_socket != INVALID_SOCKET) closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    if (g_wsaStarted) WSACleanup();
    g_wsaStarted = false;
}

} // namespace ghost
