#pragma once

// Game addresses for the Steam build of HITMAN 3.280.0.0 (see game_build.h).
// All values are RVAs relative to HITMAN3.exe. "slot N" is a vtable index.

#include <cstdint>

namespace ghost::steam::rva {

// --- Native lobby state machine (slot 5 OnEnter, 6 OnExit, 10 Update) -------
constexpr uintptr_t kLobbyIdleVtable = 0x1f1bdb0;
constexpr uintptr_t kLobbyIdleEnter = 0xff2270;
constexpr uintptr_t kLobbyIdleUpdate = 0xff2190;

constexpr uintptr_t kLobbyJoinVtable = 0x1f1cfa8;            // ZLobbyJoin
constexpr uintptr_t kLobbyJoinEnter = 0x1000d50;
constexpr uintptr_t kLobbyJoinExit = 0x1000e40;
// ZLobbyJoin's Steam LobbyEnter_t callback: (state, {lobby id, ..., +16 response}).
constexpr uintptr_t kLobbyJoinOnLobbyEnter = 0x1000f00;

constexpr uintptr_t kLobbyConnectedVtable = 0x1f1eea0;
constexpr uintptr_t kLobbyConnectedEnter = 0x10086b0;

constexpr uintptr_t kLocalHostVtable = 0x1f1ce88;            // ZLobbyCreateLocalHost
constexpr uintptr_t kLocalHostEnter = 0xfffe20;
constexpr uintptr_t kLocalHostExit = 0xffff00;

// ZGameLobbyManagerSteam slot 12 opens the Steam overlay invite dialog.
constexpr uintptr_t kSteamLobbyManagerVtable = 0x1f1d210;
constexpr uintptr_t kOpenInviteDialog = 0xff4380;

// --- Invites -----------------------------------------------------------------
// Dispatches an accepted platform invite (lobby id string + details) to the UI.
constexpr uintptr_t kInviteDispatcher = 0xff3420;
// Delegate list the dispatcher calls; the multiplayer UI registers
// kUIInviteHandler in it once the Ghost Mode menu has been opened.
constexpr uintptr_t kInviteDelegates = 0x31748c0;
constexpr uintptr_t kUIInviteHandler = 0xb67af0;

// --- Host public key -----------------------------------------------------------
constexpr uintptr_t kNetworkManager = 0x31745c0;             // +0x20: network context
constexpr uintptr_t kKeyToString = 0x155b300;

// --- Engine helpers ------------------------------------------------------------
constexpr uintptr_t kDelegateAdd = 0x6ab00;
constexpr uintptr_t kSteamApiRunCallbacksImport = 0x1b44910;  // IAT entry
constexpr uintptr_t kCodeStart = 0x1000;
constexpr uintptr_t kCodeEnd = 0x1b41200;

} // namespace ghost::steam::rva
