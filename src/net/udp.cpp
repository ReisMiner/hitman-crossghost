#include "net/udp.h"

#include <string>

namespace ghost::net {

bool ParseEndpoint(std::string_view text, sockaddr_in& address) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) return false;
    unsigned port = 0;
    for (char c : text.substr(colon + 1)) {
        if (c < '0' || c > '9') return false;
        port = port * 10 + unsigned(c - '0');
        if (port > 65535) return false;
    }
    if (port == 0) return false;
    address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    const std::string host(text.substr(0, colon));
    return InetPtonA(AF_INET, host.c_str(), &address.sin_addr) == 1;
}

bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

sockaddr_in LoopbackEndpoint(uint16_t port) {
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

SOCKET OpenNonblockingUdpSocket() {
    SOCKET result = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (result == INVALID_SOCKET) return result;
    u_long nonblocking = 1;
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    if (ioctlsocket(result, FIONBIO, &nonblocking) == SOCKET_ERROR ||
        bind(result, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        closesocket(result);
        return INVALID_SOCKET;
    }
    return result;
}

void EnlargeSocketBuffers(SOCKET socket) {
    const int size = 4 << 20;
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
}

} // namespace ghost::net
