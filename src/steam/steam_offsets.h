#pragma once

// Game addresses for the Steam build of HITMAN 3.290.1.0 (see game_build.h).
// All values are RVAs relative to HITMAN3.exe. "slot N" is a vtable index.

#include <cstdint>

namespace ghost::steam::rva {

// --- Native lobby state machine (slot 5 OnEnter, 6 OnExit, 10 Update) -------
constexpr uintptr_t kLobbyIdleVtable = 0x1f38148;
constexpr uintptr_t kLobbyIdleEnter = 0x100aae0;
constexpr uintptr_t kLobbyIdleUpdate = 0x100aa00;

constexpr uintptr_t kLobbyJoinVtable = 0x1f391d0;            // ZLobbyJoin
constexpr uintptr_t kLobbyJoinEnter = 0x10195c0;
constexpr uintptr_t kLobbyJoinExit = 0x10196b0;
// ZLobbyJoin's Steam LobbyEnter_t callback: (state, {lobby id, ..., +16 response}).
constexpr uintptr_t kLobbyJoinOnLobbyEnter = 0x1019770;

constexpr uintptr_t kLobbyConnectedVtable = 0x1f3b248;
constexpr uintptr_t kLobbyConnectedEnter = 0x1020f20;

constexpr uintptr_t kLocalHostVtable = 0x1f392f0;            // ZLobbyCreateLocalHost
constexpr uintptr_t kLocalHostEnter = 0x1018690;
constexpr uintptr_t kLocalHostExit = 0x1018770;

// ZGameLobbyManagerSteam slot 12 opens the Steam overlay invite dialog.
constexpr uintptr_t kSteamLobbyManagerVtable = 0x1f395a8;
constexpr uintptr_t kOpenInviteDialog = 0x100cbf0;

// --- Invites -----------------------------------------------------------------
// Dispatches an accepted platform invite (lobby id string + details) to the UI.
constexpr uintptr_t kInviteDispatcher = 0x100bc90;
// Delegate list the dispatcher calls; the multiplayer UI registers
// kUIInviteHandler in it once the Ghost Mode menu has been opened.
constexpr uintptr_t kInviteDelegates = 0x31952a0;
constexpr uintptr_t kUIInviteHandler = 0xb7cd40;

// --- Host public key -----------------------------------------------------------
constexpr uintptr_t kNetworkContextPointer = 0x3194fe0;     // ZNetworkManager +0x40
constexpr uintptr_t kKeyToString = 0x15757d0;

// --- Engine helpers ------------------------------------------------------------
constexpr uintptr_t kDelegateAdd = 0x6ab50;
constexpr uintptr_t kSteamApiRunCallbacksImport = 0x1b5e910;  // IAT entry
constexpr uintptr_t kCodeStart = 0x1000;
constexpr uintptr_t kCodeEnd = 0x1b5b67c;                    // .text virtual end

} // namespace ghost::steam::rva
