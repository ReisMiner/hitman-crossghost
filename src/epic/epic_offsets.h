#pragma once

// Game addresses for the Epic build of HITMAN 3.280.0.0 (see game_build.h).
// All values are RVAs relative to HITMAN3.exe. "slot N" is a vtable index.

#include <cstdint>

namespace ghost::epic::rva {

// --- Native lobby state machine --------------------------------------------
// Lobby state vtables share a layout: slot 5 OnEnter, slot 6 OnExit, slot 10 Update.
constexpr uintptr_t kLobbyIdleVtable = 0x1f11970;
constexpr uintptr_t kLobbyIdleUpdate = 0xfeef30;

constexpr uintptr_t kLocalHostVtable = 0x1f12a50;           // ZLobbyCreateLocalHost
constexpr uintptr_t kLocalHostEnter = 0xffb8c0;
constexpr uintptr_t kLocalHostExit = 0xffb9a0;
// LocalHost's update, which leaves the lobby on request 1. Reused for the
// guest's ZLobbyConnected, which has no LocalHost parent to do this.
constexpr uintptr_t kLocalHostUpdate = 0xffb820;

constexpr uintptr_t kConnectedVtable = 0x1f149a8;           // ZLobbyConnected
constexpr uintptr_t kConnectedEnter = 0x1002fb0;
constexpr uintptr_t kConnectedExit = 0x1003510;
constexpr uintptr_t kConnectedUpdate = 0x130610;            // generic no-op update
constexpr uintptr_t kConnectedStateConstructor = 0xffbae0;  // transition target

// ZMultiplayerIdle (top-level multiplayer flow). OnEnter runs when the player
// leaves multiplayer, even if the forced guest lobby never ran its OnExit.
constexpr uintptr_t kMultiplayerIdleVtable = 0x1e95dd8;
constexpr uintptr_t kMultiplayerIdleEnter = 0xd2f100;

// --- Network state machine and RakNet ---------------------------------------
constexpr uintptr_t kNetworkStatePointer = 0x3167260;       // +0x20 state, +0xa0 flag
constexpr uintptr_t kNetworkConnectingVtable = 0x1f12510;
constexpr uintptr_t kNetworkConnectingEnter = 0xff8ae0;
constexpr uintptr_t kNetworkConnectedVtable = 0x1f12398;
constexpr uintptr_t kNetworkConnectedEnter = 0xffa4d0;

constexpr uintptr_t kRakPeerVtable = 0x1f2acd0;
constexpr uintptr_t kRakPeerStartup = 0x111ac40;            // slot 6
constexpr uintptr_t kRakNetSocketVtable = 0x1f2b328;
constexpr uintptr_t kEpicSocketOverrideVtable = 0x1f147f0;
constexpr uintptr_t kReplicaManagerVtable = 0x1f14308;
constexpr uintptr_t kReplicaOnNewConnection = 0x1136440;    // slot 8

// --- Engine helpers ---------------------------------------------------------
constexpr uintptr_t kDelegateAdd = 0x698e0;
constexpr uintptr_t kDelegateRemove = 0x5e970;

// --- Multiplayer UI (guest join entry point) --------------------------------
// ZUIMultiplayerContext's invite handler queues a type-4 (join) request into
// the UI start flow, exactly like accepting a platform invite.
constexpr uintptr_t kUIStateMachineRoot = 0x312d220;
constexpr uintptr_t kUIStateMachineOffset = 0x3f8;
constexpr uintptr_t kUIContextOffset = 0x80;
constexpr uintptr_t kInviteHandler = 0xb67c80;

// --- NAT warnings ------------------------------------------------------------
constexpr uintptr_t kHostNatCheck = 0xc3eae0;               // patched to return true
constexpr uintptr_t kNatWarningPopup = 0xd306b0;            // patched to return

} // namespace ghost::epic::rva
