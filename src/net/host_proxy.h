#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ghost {

// Host-side loopback bridge between the room tunnel and native RakNet.
//
// The room client delivers guest datagrams to 127.0.0.1:<port>. The proxy
// forwards game traffic unchanged to the game's RakNet socket (UDP 30000) and
// mirrors the replies back to both of the guest's game sockets. Lobby-metadata
// queries are answered here and never reach the game.
bool StartHostProxy(uint16_t port);
void StopHostProxy();

// Forget the previous guest's endpoints, e.g. when the game recreates its socket.
void ResetHostProxyPeer();

// Lobby metadata the guest needs before it can join. It belongs to one native
// lobby and is only served once the host's UDP transport is usable.
void BeginHostLobby();
void EndHostLobby();
void SetHostLobbyTransportReady();
bool IsHostLobbyReady();
// Why the host lobby is not ready yet, for the UI; empty once it is.
std::string HostLobbyBlocker();

// Records one native lobby-data write. Unknown keys are ignored.
void PublishHostLobbyField(std::string_view key, std::string_view value);

} // namespace ghost
