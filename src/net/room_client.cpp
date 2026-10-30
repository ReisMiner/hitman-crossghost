#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <winhttp.h>

#include "game_build.h"
#include "net/room_client.h"
#include "net/room_protocol.h"
#include "net/udp.h"

#include <array>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef GHOST_SERVICE_URL
#define GHOST_SERVICE_URL ""
#endif
#ifndef GHOST_BACKEND_URL
#define GHOST_BACKEND_URL ""
#endif

namespace ghost::rooms {
namespace {

constexpr const char* kProtocol = "1";
constexpr uint64_t kHelloIntervalMs = 2000;
constexpr uint64_t kHelloRetryMs = 500;
constexpr uint64_t kConnectTimeoutMs = 10000;
constexpr uint64_t kSilenceTimeoutMs = 20000;
constexpr uint8_t kHostRole = 0;
constexpr uint8_t kGuestRole = 1;
constexpr uint8_t kRakNetChannel = 1;
constexpr uint8_t kAppChannel = 2;
constexpr uint8_t kMetadataChannel = 3;

constexpr uint8_t kRakNetOfflineMagic[16] = {0, 0xff, 0xff, 0, 0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};

std::string CanonicalOrigin(std::string value) {
    if (value.ends_with('/')) value.pop_back();
    return value;
}

struct HttpHandle {
    HINTERNET handle = nullptr;
    explicit HttpHandle(HINTERNET value) : handle(value) {
        if (!handle) throw std::runtime_error("Room service HTTP setup failed");
    }
    ~HttpHandle() { WinHttpCloseHandle(handle); }
    HttpHandle(const HttpHandle&) = delete;
};

struct UdpSocket {
    SOCKET value = INVALID_SOCKET;
    UdpSocket() {
        value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (value == INVALID_SOCKET) throw std::runtime_error("Room socket creation failed");
        net::EnlargeSocketBuffers(value);
    }
    ~UdpSocket() { closesocket(value); }
    UdpSocket(const UdpSocket&) = delete;
    void Bind(bool loopbackOnly) const {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        u_long nonblocking = 1;
        if (bind(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            ioctlsocket(value, FIONBIO, &nonblocking))
            throw std::runtime_error("Room socket bind failed");
    }
    uint16_t Port() const {
        sockaddr_in address{};
        int size = sizeof(address);
        if (getsockname(value, reinterpret_cast<sockaddr*>(&address), &size))
            throw std::runtime_error("Room socket address failed");
        return ntohs(address.sin_port);
    }
};

struct Winsock {
    Winsock() { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Room WSAStartup failed"); }
    ~Winsock() { WSACleanup(); }
};

// The room HTTP API. Requests and responses use the lobby_metadata field codec.
class RoomService {
public:
    explicit RoomService(const std::string& url) {
        if (url.empty()) throw std::runtime_error("Room service is not deployed in this build");
        const std::wstring wide(url.begin(), url.end());
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
            parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
        if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) || !parts.dwHostNameLength ||
            parts.dwUserNameLength || parts.dwPasswordLength || parts.dwExtraInfoLength ||
            (parts.dwUrlPathLength && std::wstring_view(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/"))
            throw std::runtime_error("Invalid room service origin");
        m_host.assign(parts.lpszHostName, parts.dwHostNameLength);
        m_port = parts.nPort;
        m_secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        const bool loopback = m_host == L"127.0.0.1" || m_host == L"localhost";
        if (!m_secure && !(parts.nScheme == INTERNET_SCHEME_HTTP && loopback))
            throw std::runtime_error("Room service requires HTTPS (HTTP is allowed only on loopback for development)");
    }

    metadata::Fields Post(const wchar_t* path, const metadata::Fields& fields) const {
        const auto body = metadata::Encode({true, 1, 1, fields});
        if (body.empty()) throw std::runtime_error("Invalid room request fields");
        HttpHandle session(WinHttpOpen(L"GhostCrossplay/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
        WinHttpSetTimeouts(session.handle, 3000, 3000, 3000, 3000);
        HttpHandle connection(WinHttpConnect(session.handle, m_host.c_str(), m_port, 0));
        HttpHandle request(WinHttpOpenRequest(connection.handle, L"POST", path, nullptr, nullptr, nullptr,
            m_secure ? WINHTTP_FLAG_SECURE : 0));
        const DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, const_cast<DWORD*>(&redirects), sizeof(redirects));
        if (!WinHttpSendRequest(request.handle, L"Content-Type: application/octet-stream\r\n", DWORD(-1),
                const_cast<uint8_t*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
            !WinHttpReceiveResponse(request.handle, nullptr))
            throw std::runtime_error("Cannot reach room service");

        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                nullptr, &status, &size, nullptr))
            throw std::runtime_error("Invalid room service HTTP response");
        std::vector<uint8_t> bytes;
        for (;;) {
            uint8_t buffer[4097];
            DWORD count = 0;
            if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &count)) throw std::runtime_error("Room response read failed");
            if (!count) break;
            if (bytes.size() + count > 4096) throw std::runtime_error("Room response is too large");
            bytes.insert(bytes.end(), buffer, buffer + count);
        }
        metadata::Message response;
        if (!metadata::Decode(bytes, response) || !response.response || response.nonce != 1 || response.generation != 1)
            throw std::runtime_error("Invalid room service response");
        if (status < 200 || status >= 300)
            throw std::runtime_error(response.fields.contains("Error") ? response.fields.at("Error") : "Room operation failed");
        return response.fields;
    }

    // The relay runs on the same host as the HTTP API.
    sockaddr_in ResolveRelay(uint16_t udpPort) const {
        ADDRINFOW hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        ADDRINFOW* result = nullptr;
        if (GetAddrInfoW(m_host.c_str(), nullptr, &hints, &result) || !result)
            throw std::runtime_error("Room relay DNS lookup failed");
        sockaddr_in relay = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
        FreeAddrInfoW(result);
        relay.sin_port = htons(udpPort);
        return relay;
    }

private:
    std::wstring m_host;
    INTERNET_PORT m_port = 0;
    bool m_secure = true;
};

void SendDatagram(SOCKET socket, std::span<const uint8_t> bytes, const sockaddr_in& to) {
    if (!bytes.empty())
        sendto(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

uint16_t ParsePort(std::string_view text) {
    unsigned value = 0;
    if (text.empty() || text.size() > 5) throw std::runtime_error("Invalid relay port");
    for (char c : text) {
        if (c < '0' || c > '9') throw std::runtime_error("Invalid relay port");
        value = value * 10 + (c - '0');
    }
    if (!value || value > 65535) throw std::runtime_error("Invalid relay port");
    return static_cast<uint16_t>(value);
}

bool IsRakNetConnectionRequest(std::span<const uint8_t> data) {
    return data.size() >= 18 && data[0] == 5 && !std::memcmp(data.data() + 1, kRakNetOfflineMagic, 16);
}

} // namespace

std::string ServiceUrl() {
#ifdef GHOST_DEVELOPMENT_SERVICE_OVERRIDE
    char url[512]{};
    const auto count = GetEnvironmentVariableA("GHOST_SERVICE_URL", url, sizeof(url));
    if (count && count < sizeof(url)) return std::string(url, count);
#endif
    return GHOST_SERVICE_URL;
}

struct Client::Impl {
    mutable std::mutex mutex;
    Snapshot snapshot;
    std::thread worker;
    std::atomic<bool> cancel{false}, running{false};

    void Set(State state, std::string status) {
        std::lock_guard lock(mutex);
        snapshot.state = state;
        snapshot.status = std::move(status);
        if (state == State::Failed || state == State::Idle) {
            snapshot.code.clear();
            snapshot.localEndpoint.clear();
        }
    }

    void Run(bool host, const std::string& code, uint16_t hostProxyPort, const std::string& url, const std::string& store) {
        metadata::Fields credentials;
        std::unique_ptr<RoomService> service;
        std::string failure;
        try {
            Winsock winsock;
            service = std::make_unique<RoomService>(url);
            credentials = service->Post(host ? L"/v1/rooms/create" : L"/v1/rooms/join",
                {{"Protocol", kProtocol}, {"Build", kGameVersion},
                    {"Store", store.empty() ? (host ? "epic" : "steam") : store}, {"RoomCode", code}});
            Relay(host, hostProxyPort, *service, credentials);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        // Best effort; the server's lease also releases rooms after crashes.
        if (service && credentials.contains("RoomId") && credentials.contains("Token")) {
            try {
                service->Post(L"/v1/rooms/leave", {{"RoomId", credentials.at("RoomId")}, {"Token", credentials.at("Token")}});
            } catch (...) {}
        }
        Set(cancel.load() ? State::Idle : State::Failed, cancel.load() ? "Room closed" : failure);
        running.store(false);
    }

    // Forwards datagrams between the relay and local sockets until cancelled,
    // the room closes, or the relay goes silent.
    void Relay(bool host, uint16_t hostProxyPort, const RoomService& service, const metadata::Fields& credentials) {
        if (std::string_view(GHOST_BACKEND_URL).size() &&
            CanonicalOrigin(credentials.at("BackendUrl")) != CanonicalOrigin(GHOST_BACKEND_URL))
            throw std::runtime_error("Room service/backend configuration mismatch; update GhostCrossplay");
        RoomId room;
        Token token;
        const uint8_t role = host ? kHostRole : kGuestRole;
        if (!Unhex(credentials.at("RoomId"), room) || !Unhex(credentials.at("Token"), token) ||
            credentials.at("Protocol") != kProtocol || credentials.at("Role") != (host ? "0" : "1"))
            throw std::runtime_error("Invalid room credentials");
        const auto relay = service.ResolveRelay(ParsePort(credentials.at("UdpPort")));

        Authenticator authenticator;
        uint64_t sequence = 0;
        const auto sendUp = [&](SOCKET socket, Kind kind, uint8_t channel, std::span<const uint8_t> payload = {}) {
            SendDatagram(socket, Encode(authenticator, room, token, role, kind, channel, ++sequence, payload), relay);
        };

        UdpSocket remote;
        remote.Bind(false);
        // The host uses one source socket per channel so the host proxy can
        // tell the guest's RakNet and app sockets apart. The guest exposes one.
        std::array<UdpSocket, 3> local;
        for (auto& socket : local) socket.Bind(true);
        const int localCount = host ? 3 : 1;
        const sockaddr_in hostProxy = net::LoopbackEndpoint(hostProxyPort);
        std::array<sockaddr_in, 3> guestClients{};   // last game source per channel

        ReplayWindow replay;
        uint64_t lastHello = 0, lastReply = GetTickCount64();
        bool acknowledged = false;
        while (!cancel.load()) {
            const auto now = GetTickCount64();
            if (now - lastReply > (acknowledged ? kSilenceTimeoutMs : kConnectTimeoutMs))
                throw std::runtime_error("Room relay timed out; check internet/UDP connectivity");
            if (!lastHello || now - lastHello >= (acknowledged ? kHelloIntervalMs : kHelloRetryMs)) {
                sendUp(remote.value, Kind::Hello, 0);
                lastHello = now;
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(remote.value, &readSet);
            for (int i = 0; i < localCount; ++i) FD_SET(local[i].value, &readSet);
            timeval timeout{0, 20000};
            if (select(0, &readSet, nullptr, nullptr, &timeout) == SOCKET_ERROR)
                throw std::runtime_error("Room relay socket failed");

            for (int index = -1; index < localCount; ++index) {
                const SOCKET socket = index < 0 ? remote.value : local[index].value;
                if (!FD_ISSET(socket, &readSet)) continue;
                // Bound each turn so cancellation and keepalives cannot starve.
                for (int n = 0; n < 128; ++n) {
                    uint8_t bytes[65535];
                    sockaddr_in from{};
                    int size = sizeof(from);
                    const int count = recvfrom(socket, reinterpret_cast<char*>(bytes), sizeof(bytes), 0,
                        reinterpret_cast<sockaddr*>(&from), &size);
                    if (count == SOCKET_ERROR) break;
                    if (!count) continue;
                    const std::span<const uint8_t> data(bytes, static_cast<size_t>(count));

                    if (index < 0) {
                        // From the relay.
                        Packet packet;
                        if (!net::SameEndpoint(from, relay) || !Decode(authenticator, room, token, role, data, packet) ||
                            !IsValidDownstream(packet) || !replay.Accept(packet.sequence)) continue;
                        lastReply = now;
                        if (packet.kind == Kind::Gone) throw std::runtime_error("Room closed or expired");
                        if (packet.kind == Kind::Ack && !acknowledged) {
                            acknowledged = true;
                            std::lock_guard lock(mutex);
                            snapshot = {State::Ready, host ? "Room ready; share the code" : "Room connected; joining native lobby",
                                credentials.at("RoomCode"), "127.0.0.1:" + std::to_string(local[0].Port()),
                                credentials.at("BackendUrl")};
                        }
                        if (packet.kind == Kind::Down) {
                            if (host) SendDatagram(local[packet.channel - 1].value, packet.payload, hostProxy);
                            else if (guestClients[packet.channel - 1].sin_port)
                                SendDatagram(local[0].value, packet.payload, guestClients[packet.channel - 1]);
                        }
                    } else if (acknowledged) {
                        // From the local game (guest) or host proxy (host).
                        uint8_t channel;
                        if (host) {
                            if (!net::SameEndpoint(from, hostProxy)) continue;
                            channel = static_cast<uint8_t>(index + 1);
                        } else {
                            if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
                            channel = ClassifyGuestDatagram(data, from, guestClients[0]);
                            guestClients[channel - 1] = from;
                        }
                        sendUp(remote.value, Kind::Up, channel, data);
                    }
                }
            }
        }
    }

    static bool IsValidDownstream(const Packet& packet) {
        if (packet.kind == Kind::Down)
            return packet.channel >= 1 && packet.channel <= 3 && !packet.payload.empty();
        return (packet.kind == Kind::Ack || packet.kind == Kind::Gone) && packet.channel == 0 && packet.payload.empty();
    }

    // The guest game uses two sockets (RakNet on :30000 and an app socket) plus
    // our metadata queries; keep them on separate channels.
    static uint8_t ClassifyGuestDatagram(std::span<const uint8_t> data, const sockaddr_in& from, const sockaddr_in& knownRakNet) {
        if (metadata::IsControl(data)) return kMetadataChannel;
        if (IsRakNetConnectionRequest(data) || net::SameEndpoint(from, knownRakNet) || ntohs(from.sin_port) == 30000)
            return kRakNetChannel;
        return kAppChannel;
    }
};

Client::Client() : m_impl(std::make_unique<Impl>()) {}
Client::~Client() { Stop(); }

bool Client::Start(bool host, std::string code, uint16_t hostProxyPort, std::string serviceUrl, std::string store) {
    if (m_impl->running.exchange(true)) return false;
    if (m_impl->worker.joinable()) m_impl->worker.join();
    m_impl->cancel.store(false);
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->snapshot = {State::Connecting, host ? "Creating room..." : "Looking up room..."};
    }
    m_impl->worker = std::thread(&Impl::Run, m_impl.get(), host, std::move(code), hostProxyPort,
        serviceUrl.empty() ? ServiceUrl() : std::move(serviceUrl), std::move(store));
    return true;
}

void Client::Cancel() { m_impl->cancel.store(true); }

void Client::Stop() {
    Cancel();
    if (m_impl->worker.joinable()) m_impl->worker.join();
}

Snapshot Client::Get() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->snapshot;
}

} // namespace ghost::rooms
