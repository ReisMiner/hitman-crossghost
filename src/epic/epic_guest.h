#pragma once

#include "common/memory.h"
#include "net/deferred_room_close.h"
#include "net/lobby_metadata.h"
#include "net/metadata_fetcher.h"
#include "net/room_client.h"

#include <atomic>
#include <mutex>
#include <string>

namespace ghost::epic {

// Joining a room from the Epic build.
//
// Flow:
//  1. Join(): connect to the room; the room client exposes a loopback endpoint.
//  2. Fetch the host's lobby metadata (map, key, version) over that endpoint.
//  3. Queue a native "accept invite" in the multiplayer UI, which drives the
//     game into its join flow (lobby request 4).
//  4. On the next ZLobbyIdle update, redirect the lobby to ZLobbyConnected and
//     register a lobby-data provider that answers with the host's metadata.
//     The native join routine then connects RakNet to the loopback endpoint.
//  5. Leaving the lobby or multiplayer ends the session and closes the room.
//
// All methods run on the game thread except where noted.
class Guest {
public:
    Guest(rooms::Client& room, DeferredRoomClose& roomClose) : m_room(room), m_roomClose(roomClose) {}

    // UI thread. Returns false if the previous room is still closing.
    bool Join(const std::string& code);
    // UI thread: Host pressed; stop any join in progress.
    void CancelForHosting() { m_requested.store(false); m_ready.store(false); }
    // UI thread: "Close room" pressed.
    void Abandon() { m_requested.store(false); }

    bool Requested() const { return m_requested.load(std::memory_order_acquire); }

    // ZLobbyIdle::Update, before the original. Advances the room/metadata steps.
    void Tick();
    // ZLobbyIdle::Update, before the original. Switches the pending request to
    // "join" once metadata is ready. Returns true if the transition produced by
    // the original update must be redirected to ZLobbyConnected.
    bool TakeJoinRequest(void* idleState, int& request);

    void OnLobbyConnectedEnter(void* state);
    void OnLobbyConnectedExit(void* state);
    bool IsGuestLobby(void* state) const { return state != nullptr && state == m_lobbyState.load(std::memory_order_acquire); }
    void OnMultiplayerIdleEnter();

    // Shutdown: stop polling for metadata.
    void Stop();

private:
    // Native lobby-data provider callback (registered with the lobby context).
    static void __fastcall ProvideLobbyData(void* self, GameString* key, GameString* out);
    static Guest* s_instance;

    void StartMetadataFetch();
    void PollMetadata();
    bool RegisterProvider(void* lobbyContext);
    bool QueueInviteAccept();
    void EndSession(bool requestNativeLeave);

    rooms::Client& m_room;
    DeferredRoomClose& m_roomClose;
    net::MetadataFetcher m_fetcher;
    std::mutex m_dataMutex;
    metadata::Fields m_lobbyData;     // what the provider returns to the game
    std::string m_endpoint;

    std::atomic<bool> m_requested{false};       // Join pressed; room in progress
    std::atomic<bool> m_ready{false};           // metadata received
    std::atomic<bool> m_joinIssued{false};      // lobby request switched to join
    std::atomic<bool> m_inviteQueued{false};    // multiplayer UI join queued
    std::atomic<void*> m_providerContext{nullptr};
    std::atomic<void*> m_lobbyState{nullptr};   // our forced ZLobbyConnected
};

} // namespace ghost::epic
