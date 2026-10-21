#pragma once

#include "net/lobby_metadata.h"
#include "net/udp.h"

namespace ghost::steam::shim {

// Lets the Steam build join a non-Steam host by impersonating a Steam lobby.
//
// While active, Steam lobby id 0 is a fake lobby owned by fake member 1 (the
// host) whose lobby data is the host's metadata. Steam P2P traffic to member 1
// is sent over UDP to the room tunnel endpoint instead of through Steam.
// Every other lobby and peer is passed through to Steam unchanged.
//
// The interface hooks are installed once and stay installed; they only change
// behavior while the shim is active.

constexpr uint64_t kLobbyId = 0;
constexpr uint64_t kHostMemberId = 1;

// Hooks ISteamMatchmaking009 and ISteamNetworking006. Idempotent.
bool Install();
void Uninstall();

// Opens the UDP socket used for P2P traffic to the host.
bool OpenRelay(const sockaddr_in& tunnelEndpoint);

// Makes lobby 0 visible to the game with the given host lobby data.
void Activate(metadata::Fields hostLobbyData);
// Turns the fake lobby off and closes the relay socket.
void Deactivate();
bool Active();

} // namespace ghost::steam::shim
