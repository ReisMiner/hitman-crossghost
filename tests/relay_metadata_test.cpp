#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include "net/host_proxy.h"
#include "net/lobby_metadata.h"

#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Socket {
    SOCKET value = INVALID_SOCKET;
    explicit Socket(uint16_t port = 0) {
        value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        const BOOL exclusive = TRUE;
        setsockopt(value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        if (value == INVALID_SOCKET || bind(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            if (value != INVALID_SOCKET) closesocket(value);
            throw std::runtime_error("test port unavailable; close HITMAN before running relay integration tests");
        }
    }
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
    uint16_t Port() const {
        sockaddr_in address = {};
        int size = sizeof(address);
        Require(getsockname(value, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
        return ntohs(address.sin_port);
    }
    void Send(const std::vector<uint8_t>& packet, uint16_t port) const {
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        Require(sendto(value, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == static_cast<int>(packet.size()), "sendto");
    }
    std::vector<uint8_t> Receive(int timeoutMs = 1000) const {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(value, &set);
        timeval timeout = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        const int available = select(0, &set, nullptr, nullptr, &timeout);
        Require(available != SOCKET_ERROR, "select");
        if (available == 0) return {};
        std::vector<uint8_t> result(65535);
        const int count = recv(value, reinterpret_cast<char*>(result.data()), static_cast<int>(result.size()), 0);
        Require(count >= 0, "recv");
        result.resize(count);
        return result;
    }
};

void TestCodec() {
    using namespace ghost::metadata;
    Message request{false, 0x123456789abcdef0};
    const auto encoded = Encode(request);
    Require(encoded.size() == 25 && encoded[17] == 0xf0 && encoded[24] == 0x12, "request wire format");
    Message result;
    Require(Decode(encoded, result) && result.nonce == request.nonce && !result.response, "request decode");
    Message response{true, request.nonce, 9, {{"ContractId", "contract"}, {"HostPublicKey", std::string(88, 'K')}}};
    const auto bytes = Encode(response);
    Require(Decode(bytes, result) && result.fields == response.fields && result.generation == 9, "snapshot roundtrip");
    for (size_t i = 0; i < bytes.size(); ++i) {
        Message unchanged{false, 42};
        Require(!Decode(std::span(bytes).first(i), unchanged) && unchanged.nonce == 42, "truncated response mutated snapshot");
    }
    auto bad = bytes;
    bad.push_back(0);
    Require(!Decode(bad, result), "trailing bytes accepted");
    bad = bytes;
    bad[16] = 3;
    Require(!Decode(bad, result), "unknown message accepted");
    bad = Encode({true, request.nonce, 1, {{"a", "b"}}});
    const std::vector<uint8_t> field(bad.begin() + 34, bad.end());
    bad[33] = 2;
    bad.insert(bad.end(), field.begin(), field.end());
    Require(!Decode(bad, result), "duplicate fields accepted");
    response.fields["oversized"] = std::string(kMaxValue + 1, 'x');
    Require(Encode(response).empty(), "oversized field encoded");
    Fields pending{{"ContractId", ""}, {"PresetId", "preset"}, {"gameMode", "versus"},
        {"HostPublicKey", "key"}, {"HostGameVersion", "3.290.1.0"}};
    Require(!Missing(pending).empty(), "unresolved match advertised as ready");
    pending["matchId"] = "00000000-0000-0000-0000-000000000000";
    Require(!Missing(pending).empty(), "nil match advertised as joinable");
    pending["matchId"] = "0b6f1ef8-17bb-4aca-a131-fc6ad9dcec70";
    Require(Missing(pending).empty(), "native preset/match resolution was blocked");
    pending.erase("matchId");
    pending["ContractId"] = "contract";
    Require(Missing(pending).empty(), "known contract resolution was blocked");
    Require(!IsControl(std::vector<uint8_t>{0x84, 0x01, 0x02, 0x03}), "RakNet data treated as control");
}

void TestRelay() {
    using namespace ghost;
    Socket native(30000), raknet, app, control;
    Socket reservation;
    const uint16_t port = reservation.Port();
    closesocket(reservation.value);
    reservation.value = INVALID_SOCKET;
    Require(StartHostProxy(port), "start relay");
    const auto query = metadata::Encode({false, 73});
    control.Send(query, port);
    metadata::Message response;
    Require(metadata::Decode(control.Receive(), response) && response.nonce == 73 && response.generation == 0,
        "inactive lobby advertised metadata");
    BeginHostLobby();
    for (auto [key, value] : metadata::Fields{{"ContractId", "contract"}, {"PresetId", "preset"},
        {"gameMode", "versus"}, {"HostPublicKey", "current-key"}, {"gamever_actual-host", "3.290.1.0"}})
        PublishHostLobbyField(key, value);
    SetHostLobbyTransportReady();
    control.Send(query, port);
    Require(metadata::Decode(control.Receive(), response) && response.generation != 0 && metadata::Missing(response.fields).empty(),
        "ready lobby snapshot missing fields");
    const auto generation = response.generation;
    Require(native.Receive(60).empty(), "control datagram leaked into native RakNet");

    const std::vector<uint8_t> handshake = {0x05, 0, 0xff, 0xff, 0, 0xfe, 0xfe, 0xfe, 0xfe,
        0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78, 6};
    raknet.Send(handshake, port);
    Require(native.Receive() == handshake, "native handshake was changed");
    const std::vector<uint8_t> encrypted = {0xad, 0xc9, 0x06, 0x02, 0x8b, 0xec, 0xf6, 0x28};
    app.Send(encrypted, port);
    Require(native.Receive() == encrypted, "opaque app payload was changed");
    control.Send(query, port);
    Require(metadata::Decode(control.Receive(), response), "metadata query failed during traffic");
    const std::vector<uint8_t> reply = {0xc0, 1, 0, 1, 0, 0, 0};
    native.Send(reply, port);
    Require(raknet.Receive() == reply && app.Receive() == reply, "native response not mirrored to both endpoints");
    Require(control.Receive(60).empty(), "control socket became a game endpoint");
    auto malformed = query;
    malformed.push_back(0);
    app.Send(malformed, port);
    Require(native.Receive(60).empty(), "malformed control leaked into native traffic");

    EndHostLobby();
    control.Send(query, port);
    Require(metadata::Decode(control.Receive(), response) && response.generation == 0 && response.fields.empty(),
        "ended lobby exposed stale metadata");
    BeginHostLobby();
    SetHostLobbyTransportReady();
    ResetHostProxyPeer();
    control.Send(query, port);
    Require(metadata::Decode(control.Receive(), response) && response.generation != generation && response.fields.empty(),
        "new lobby reused old metadata");
    native.Send(reply, port);
    Require(raknet.Receive(60).empty() && app.Receive(60).empty(), "new lobby reused stale game endpoints");
    StopHostProxy();
}
} // namespace

int main() {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    try {
        TestCodec();
        TestRelay();
        WSACleanup();
        std::cout << "PASS: metadata validation, control isolation, opaque dual-endpoint relay, lobby reset\n";
        return 0;
    } catch (const std::exception& e) {
        ghost::StopHostProxy();
        WSACleanup();
        std::cerr << e.what() << '\n';
        return 1;
    }
}
