#include "net/metadata_fetcher.h"

#include <Windows.h>
#include <bcrypt.h>

namespace ghost::net {

bool MetadataFetcher::Start(const std::string& endpoint, std::string& error) {
    Stop();
    m_winsock = std::make_unique<WinsockSession>();
    if (!m_winsock->Started()) { error = "WSAStartup failed"; Stop(); return false; }
    if (!ParseEndpoint(endpoint, m_endpoint) ||
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&m_nonce), sizeof(m_nonce),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || m_nonce == 0) {
        error = "invalid IPv4:port or nonce generation failed";
        Stop();
        return false;
    }
    m_socket = OpenNonblockingUdpSocket();
    if (m_socket == INVALID_SOCKET) { error = "socket creation failed"; Stop(); return false; }
    m_started = GetTickCount64();
    m_lastQuery = 0;
    m_waiting.clear();
    return true;
}

void MetadataFetcher::Stop() {
    if (m_socket != INVALID_SOCKET) closesocket(m_socket);
    m_socket = INVALID_SOCKET;
    m_winsock.reset();
}

MetadataFetcher::Progress MetadataFetcher::Poll(metadata::Fields& fields) {
    if (!Active()) return Progress::Pending;
    const uint64_t now = GetTickCount64();
    if (now - m_started > kTimeoutMs) {
        Stop();
        return Progress::TimedOut;
    }
    if (m_lastQuery == 0 || now - m_lastQuery >= kQueryIntervalMs) {
        const auto request = metadata::Encode({false, m_nonce});
        sendto(m_socket, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0,
            reinterpret_cast<const sockaddr*>(&m_endpoint), sizeof(m_endpoint));
        m_lastQuery = now;
    }

    Progress progress = Progress::Pending;
    for (int i = 0; i < 16; ++i) {
        uint8_t buffer[metadata::kMaxPacket + 1];
        sockaddr_in source = {};
        int sourceSize = sizeof(source);
        const int length = recvfrom(m_socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceSize);
        if (length == SOCKET_ERROR) break;
        metadata::Message response;
        if (length <= 0 || !SameEndpoint(source, m_endpoint) ||
            !metadata::Decode({buffer, static_cast<size_t>(length)}, response) ||
            !response.response || response.nonce != m_nonce) continue;

        const std::string missing = response.generation == 0
            ? "host lobby/transport not ready" : metadata::Missing(response.fields);
        if (!missing.empty()) {
            if (missing != m_waiting) { m_waiting = missing; progress = Progress::WaitingChanged; }
            continue;
        }
        fields = std::move(response.fields);
        Stop();
        return Progress::Ready;
    }
    return progress;
}

} // namespace ghost::net
