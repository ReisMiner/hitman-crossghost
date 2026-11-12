#pragma once

// Game addresses for the Epic build of HITMAN 3.290.1.0 (see game_build.h).
// All values are RVAs relative to HITMAN3.exe. "slot N" is a vtable index.

#include <cstdint>

namespace ghost::epic::rva {

// --- Native lobby state machine --------------------------------------------
// Lobby state vtables share a layout: slot 5 OnEnter, slot 6 OnExit, slot 10 Update.
constexpr uintptr_t kLobbyIdleVtable = 0x1f2de68;
constexpr uintptr_t kLobbyIdleUpdate = 0x10077c0;

constexpr uintptr_t kLocalHostVtable = 0x1f2efe8;           // ZLobbyCreateLocalHost
constexpr uintptr_t kLocalHostEnter = 0x1014150;
constexpr uintptr_t kLocalHostExit = 0x1014230;
// LocalHost's update, which leaves the lobby on request 1. Reused for the
// guest's ZLobbyConnected, which has no LocalHost parent to do this.
constexpr uintptr_t kLocalHostUpdate = 0x10140b0;

constexpr uintptr_t kConnectedVtable = 0x1f30e78;           // ZLobbyConnected
constexpr uintptr_t kConnectedEnter = 0x101b840;
constexpr uintptr_t kConnectedExit = 0x101bda0;
constexpr uintptr_t kConnectedUpdate = 0x1139a0;            // generic no-op update
constexpr uintptr_t kConnectedStateConstructor = 0x1014370; // transition target

// ZMultiplayerIdle (top-level multiplayer flow). OnEnter runs when the player
// leaves multiplayer, even if the forced guest lobby never ran its OnExit.
constexpr uintptr_t kMultiplayerIdleVtable = 0x1eb18a0;
constexpr uintptr_t kMultiplayerIdleEnter = 0xd45440;

// --- Network state machine and RakNet ---------------------------------------
constexpr uintptr_t kNetworkStatePointer = 0x3187ec0;       // +0x20 state, +0xa0 flag
constexpr uintptr_t kNetworkConnectingVtable = 0x1f2e9f0;
constexpr uintptr_t kNetworkConnectingEnter = 0x1011370;
constexpr uintptr_t kNetworkConnectedVtable = 0x1f2e850;
constexpr uintptr_t kNetworkConnectedEnter = 0x1012d60;

constexpr uintptr_t kRakPeerVtable = 0x1f47058;
constexpr uintptr_t kRakPeerStartup = 0x11334d0;            // slot 1 (slot 6 is now IsInSecurityExceptionList)
constexpr uintptr_t kRakNetSocketVtable = 0x1f476f0;         // RNS2_Windows
constexpr uintptr_t kEpicSocketOverrideVtable = 0x1f30d50;  // primary vtable; the socket stores the object itself
// ReplicaManager3 (ZReplicaManager). OnNewConnection only creates a replication
// connection when +0x38 (auto-create) is set; natively the game creates them
// when the FullyConnectedMesh2 join completes. +0x50 world: +0x00 connection
// array, +0x08 count; connection +0x20 remote RakNet GUID.
constexpr uintptr_t kReplicaManagerVtable = 0x1f307e8;
constexpr uintptr_t kReplicaOnNewConnection = 0x114ecd0;    // slot 8
constexpr size_t kReplicaAutoCreate = 0x38;
constexpr size_t kReplicaWorld = 0x50;
constexpr size_t kReplicaConnectionGuid = 0x20;

// Network object pointer: +0x13 session active (set when the mesh join is
// accepted; until then the game drops replicas it receives).
constexpr uintptr_t kNetworkObject = 0x3187eb0;
constexpr size_t kNetworkSessionActive = 0x13;

// --- Engine helpers ---------------------------------------------------------
constexpr uintptr_t kDelegateAdd = 0x69950;
constexpr uintptr_t kDelegateRemove = 0x5e9e0;

// --- Multiplayer UI (guest join entry point) --------------------------------
// ZUIMultiplayerContext's invite handler queues a type-4 (join) request into
// the UI start flow, exactly like accepting a platform invite.
constexpr uintptr_t kUIStateMachineRoot = 0x314dac0;
constexpr uintptr_t kUIStateMachineOffset = 0x3f8;
constexpr uintptr_t kUIContextOffset = 0x80;
constexpr uintptr_t kInviteHandler = 0xb7cef0;

// --- NAT warnings ------------------------------------------------------------
constexpr uintptr_t kHostNatCheck = 0xc53d80;               // patched to return true
constexpr uintptr_t kNatWarningPopup = 0xd469f0;            // patched to return

} // namespace ghost::epic::rva
