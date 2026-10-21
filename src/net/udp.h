#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string_view>

namespace ghost::net {

// Owns one WSAStartup/WSACleanup pair.
class WinsockSession {
public:
    WinsockSession() { WSADATA data = {}; m_started = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinsockSession() { if (m_started) WSACleanup(); }
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
    bool Started() const { return m_started; }
private:
    bool m_started = false;
};

// Parses "a.b.c.d:port" (IPv4 only, non-zero port).
bool ParseEndpoint(std::string_view text, sockaddr_in& address);

bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b);

sockaddr_in LoopbackEndpoint(uint16_t port);

// Non-blocking UDP socket bound to an ephemeral port on all interfaces.
SOCKET OpenNonblockingUdpSocket();

// Raises the kernel socket buffers. The 64 KB default overflows during
// gameplay bursts and forces RakNet resends, which shows up as stutter.
void EnlargeSocketBuffers(SOCKET socket);

} // namespace ghost::net
