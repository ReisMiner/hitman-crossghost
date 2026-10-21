#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ghost::rooms {

enum class State { Idle, Connecting, Ready, Failed };

struct Snapshot {
    State state = State::Idle;
    std::string status = "Room service is idle";
    std::string code;
    // Guest only: loopback "127.0.0.1:port" that tunnels to the host.
    std::string localEndpoint;
    std::string backendUrl;
};

inline bool IsBusy(State state) { return state == State::Connecting || state == State::Ready; }

// A room on the LocalGhost relay. The worker thread does HTTPS, DNS, keepalives
// and UDP forwarding; the game and UI threads only call Start/Cancel/Get.
//
// Host: datagrams from the guest are delivered to the loopback host proxy on
// `hostProxyPort` (one source socket per channel), replies go back the same way.
// Guest: the game talks to Snapshot::localEndpoint; channel 1 carries RakNet,
// channel 2 the second game socket and channel 3 lobby-metadata queries.
class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Returns false while a previous room is still shutting down.
    // `store` is "epic" or "steam"; `serviceUrl` defaults to the built-in origin.
    bool Start(bool host, std::string code = {}, uint16_t hostProxyPort = 30001,
        std::string serviceUrl = {}, std::string store = {});
    void Cancel();
    void Stop();     // Cancel and wait for the worker thread.
    Snapshot Get() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

std::string ServiceUrl();

} // namespace ghost::rooms
