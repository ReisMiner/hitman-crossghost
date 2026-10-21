#pragma once

// Guest side of the lobby metadata exchange: polls the host (through the room
// tunnel's loopback endpoint) until it reports a complete, joinable lobby.

#include "net/lobby_metadata.h"
#include "net/udp.h"

#include <memory>
#include <string>

namespace ghost::net {

class MetadataFetcher {
public:
    enum class Progress { Pending, WaitingChanged, Ready, TimedOut };

    ~MetadataFetcher() { Stop(); }

    // Begins polling `endpoint` ("127.0.0.1:port"). Returns false and sets
    // `error` if the socket or nonce cannot be created.
    bool Start(const std::string& endpoint, std::string& error);
    void Stop();
    bool Active() const { return m_socket != INVALID_SOCKET; }

    // Non-blocking; call once per game tick. On Ready, `fields` holds the
    // host's lobby data and the fetcher has stopped.
    Progress Poll(metadata::Fields& fields);

    // What the host is still missing, e.g. "PresetId, gameMode".
    const std::string& Waiting() const { return m_waiting; }
    const sockaddr_in& Endpoint() const { return m_endpoint; }

private:
    static constexpr uint64_t kTimeoutMs = 15000;
    static constexpr uint64_t kQueryIntervalMs = 500;

    std::unique_ptr<WinsockSession> m_winsock;
    SOCKET m_socket = INVALID_SOCKET;
    sockaddr_in m_endpoint = {};
    uint64_t m_nonce = 0;
    uint64_t m_started = 0;
    uint64_t m_lastQuery = 0;
    std::string m_waiting;
};

} // namespace ghost::net
