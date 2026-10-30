#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include "net/room_client.h"
#include "net/room_protocol.h"
#include "net/host_proxy.h"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Socket {
    SOCKET value;
    explicit Socket(uint16_t port = 0) : value(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        BOOL exclusive = TRUE;
        setsockopt(value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        Require(value != INVALID_SOCKET && !bind(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)), "test UDP port unavailable; close HITMAN");
    }
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
    uint16_t Port() const {
        sockaddr_in address{}; int length = sizeof(address);
        Require(!getsockname(value, reinterpret_cast<sockaddr*>(&address), &length), "test getsockname"); return ntohs(address.sin_port);
    }
    void Send(const std::vector<uint8_t>& bytes, uint16_t port) {
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(port);
        Require(sendto(value, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr*>(&to), sizeof(to)) == bytes.size(), "test send");
    }
    std::vector<uint8_t> Receive(int ms = 2000) {
        fd_set read; FD_ZERO(&read); FD_SET(value, &read); timeval timeout{ms / 1000, ms % 1000 * 1000};
        const int ready = select(0, &read, nullptr, nullptr, &timeout); Require(ready != SOCKET_ERROR, "test select");
        if (!ready) return {};
        std::vector<uint8_t> bytes(65535); int count = recv(value, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
        Require(count >= 0, "test recv"); bytes.resize(count); return bytes;
    }
};
ghost::rooms::Snapshot Wait(ghost::rooms::Client& client, ghost::rooms::State state) {
    const auto deadline = GetTickCount64() + 6000;
    while (GetTickCount64() < deadline) {
        auto snapshot = client.Get();
        if (snapshot.state == state) return snapshot;
        if (snapshot.state == ghost::rooms::State::Failed) throw std::runtime_error(snapshot.status);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("native tunnel timed out waiting for state");
}
void Protocol() {
    using namespace ghost::rooms;
    Authenticator auth; RoomId id{}; Token token{};
    const auto bytes = Encode(auth, id, token, 0, Kind::Up, 1, 7, std::array<uint8_t, 3>{0x80, 0, 0xff});
    Packet packet;
    Require(bytes.size() == 67 && Decode(auth, id, token, 0, bytes, packet) && packet.sequence == 7, "native HMAC roundtrip");
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto damaged = bytes; damaged[i] ^= 1;
        Require(!Decode(auth, id, token, 0, damaged, packet), "modified authenticated byte accepted");
    }
    Require(!Decode(auth, id, token, 1, bytes, packet), "wrong member accepted");
    ReplayWindow replay;
    Require(replay.Accept(3) && replay.Accept(1) && replay.Accept(2) && !replay.Accept(2) &&
        replay.Accept(70) && !replay.Accept(3) && replay.Accept(69) && !replay.Accept(0), "native replay window");
}
void Integration(const std::string& url, const std::string& expectedBackend) {
    using namespace ghost;
    Socket native(30000), raknet, app, control, reservation;
    const auto proxyPort = reservation.Port(); closesocket(reservation.value); reservation.value = INVALID_SOCKET;
    Require(StartHostProxy(proxyPort), "start native host proxy");
    BeginHostLobby();
    for (const auto& [key, value] : metadata::Fields{{"PresetId", "miami"}, {"matchId", "current-match"},
        {"HostGameVersion", "3.290.1.0"}, {"HostPublicKey", "native-key"}, {"gameMode", "versus"}}) PublishHostLobbyField(key, value);
    SetHostLobbyTransportReady();
    rooms::Client host, guest;
    Require(host.Start(true, {}, proxyPort, url), "start host room");
    const auto room = Wait(host, rooms::State::Ready);
    Require(room.code.size() == 5, "room code missing");
    Require(guest.Start(false, room.code, proxyPort, url), "start guest room");
    auto joined = Wait(guest, rooms::State::Ready);
    Require(joined.backendUrl == expectedBackend, "automatic backend discovery");
    auto port = static_cast<uint16_t>(std::stoi(joined.localEndpoint.substr(joined.localEndpoint.find(':') + 1)));
    const auto query = metadata::Encode({false, 923});
    control.Send(query, port);
    metadata::Message response;
    Require(metadata::Decode(control.Receive(), response) && response.nonce == 923 && metadata::Missing(response.fields).empty(), "metadata through room tunnel");
    Require(native.Receive(60).empty(), "metadata leaked into native socket");
    const std::vector<uint8_t> handshake{5, 0, 0xff, 0xff, 0, 0xfe, 0xfe, 0xfe, 0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78, 6};
    raknet.Send(handshake, port); Require(native.Receive() == handshake, "handshake changed by outbound tunnels");
    const std::vector<uint8_t> opaque{0xad, 9, 4, 0, 0xff, 37};
    app.Send(opaque, port); Require(native.Receive() == opaque, "app payload changed by outbound tunnels");
    native.Send(opaque, proxyPort);
    Require(raknet.Receive() == opaque && app.Receive() == opaque, "native reply not delivered to both guest endpoints");
    Require(control.Receive(60).empty(), "data leaked to metadata socket");
    guest.Stop();
    Require(guest.Start(false, room.code, proxyPort, url), "rejoin start");
    joined = Wait(guest, rooms::State::Ready);
    port = static_cast<uint16_t>(std::stoi(joined.localEndpoint.substr(joined.localEndpoint.find(':') + 1)));
    control.Send(query, port); Require(metadata::Decode(control.Receive(), response), "rejoin metadata");
    raknet.Send(handshake, port); Require(native.Receive() == handshake, "rejoin handshake");
    host.Stop();
    const auto deadline = GetTickCount64() + 2000;
    while (guest.Get().state != rooms::State::Failed && GetTickCount64() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Require(guest.Get().state == rooms::State::Failed, "host closure did not end guest room");
    guest.Stop(); StopHostProxy();
}
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: GhostRoomTests SERVICE_ORIGIN [EXPECTED_BACKEND_ORIGIN]\n";
        return 2;
    }
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
    try {
        Protocol(); Integration(argv[1], argc == 3 ? argv[2] : "http://localhost:8080"); WSACleanup();
        std::cout << "PASS: C++/Node authenticated room relay, native/app/control isolation, opaque payloads, rejoin and host closure\n";
        return 0;
    } catch (const std::exception& error) {
        ghost::StopHostProxy(); WSACleanup(); std::cerr << error.what() << '\n'; return 1;
    }
}
