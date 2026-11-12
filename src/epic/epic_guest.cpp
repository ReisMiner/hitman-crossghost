#include "epic/epic_guest.h"

#include "common/diag_log.h"
#include "common/native_lobby.h"
#include "epic/epic_offsets.h"
#include "game_maps.h"

#include <set>

namespace ghost::epic {
namespace {

// The lobby context's list of lobby-data providers (queried by key).
constexpr size_t kLobbyDataProvidersOffset = 0x170;

// Network state value the game writes when leaving a lobby (ZLobbyConnected::OnExit).
constexpr int kNetworkStateDisconnect = 7;

const char* RoomStateName(rooms::State state) {
    switch (state) {
        case rooms::State::Idle: return "Idle";
        case rooms::State::Connecting: return "Connecting";
        case rooms::State::Ready: return "Ready";
        case rooms::State::Failed: return "Failed";
    }
    return "?";
}

// Logs each distinct provider answer once per session; the game queries often.
std::mutex g_providerLogMutex;
std::set<std::string> g_providerLogged;

void LogProviderAnswer(const std::string& key, const char* outcome, std::string_view value) {
    std::string line = "guest: lobby data query '" + key + "' -> " + outcome;
    if (!value.empty()) line += " '" + diag::Brief(value) + "'";
    std::lock_guard lock(g_providerLogMutex);
    if (g_providerLogged.insert(line).second) diag::Write(line);
}

} // namespace

Guest* Guest::s_instance = nullptr;

bool Guest::Join(const std::string& code) {
    diag::Log("guest: Join pressed, code '{}'", code);
    { std::lock_guard lock(g_providerLogMutex); g_providerLogged.clear(); }
    m_requested.store(true);
    m_ready.store(false);
    m_joinIssued.store(false);
    m_inviteQueued.store(false);
    if (m_roomClose.TakePending()) m_room.Cancel();
    { std::lock_guard lock(m_dataMutex); m_lobbyData.clear(); }
    if (!m_room.Start(false, code, 0, {}, "epic")) {
        m_requested.store(false);
        return false;
    }
    return true;
}

void Guest::Tick() {
    if (!Requested()) return;
    const auto room = m_room.Get();
    static rooms::State lastState = rooms::State::Idle;
    if (room.state != lastState) {
        diag::Log("guest: room {} -> {} ({}), endpoint '{}'", RoomStateName(lastState), RoomStateName(room.state),
            room.status, room.localEndpoint);
        lastState = room.state;
    }
    if (room.state == rooms::State::Ready && !m_fetcher.Active() && !m_ready.load()) {
        StartMetadataFetch();
    } else if (room.state == rooms::State::Failed || room.state == rooms::State::Idle) {
        m_requested.store(false);
        m_joinIssued.store(false);
        m_inviteQueued.store(false);
        m_fetcher.Stop();
        m_ready.store(false, std::memory_order_release);
    }
    PollMetadata();
}

void Guest::StartMetadataFetch() {
    const auto room = m_room.Get();
    if (room.state != rooms::State::Ready || room.localEndpoint.empty()) return;
    m_endpoint = room.localEndpoint;
    std::string error;
    const bool started = m_fetcher.Start(m_endpoint, error);
    diag::Log("guest: metadata fetch from {} {}{}", m_endpoint, started ? "started" : "failed: ", error);
}

void Guest::PollMetadata() {
    metadata::Fields fields;
    switch (m_fetcher.Poll(fields)) {
        case net::MetadataFetcher::Progress::TimedOut:
            diag::Log("guest: metadata fetch timed out (host still missing: '{}')", m_fetcher.Waiting());
            // Retried on the next tick while the room is still up.
            m_ready.store(false, std::memory_order_release);
            return;
        case net::MetadataFetcher::Progress::WaitingChanged:
            diag::Log("guest: host lobby not ready yet, missing: {}", m_fetcher.Waiting());
            return;
        case net::MetadataFetcher::Progress::Ready:
            break;
        default:
            return;
    }
    diag::Log("guest: host metadata received ({} fields):", fields.size());
    for (const auto& [key, value] : fields) diag::Log("guest:   host field {} = '{}'", key, diag::Brief(value));
    // The native Joining state uses both the IP and the port. Point it at the
    // tunnel so incoming and outgoing RakNet addresses agree.
    fields["HostAddress"] = m_endpoint;
    const bool hadContract = fields.contains("ContractId") && !fields["ContractId"].empty();
    FillContractFromPreset(fields);
    if (!hadContract) diag::Log("guest: ContractId filled from PresetId: '{}'", fields["ContractId"]);
    { std::lock_guard lock(m_dataMutex); m_lobbyData = std::move(fields); }
    m_ready.store(true, std::memory_order_release);
    if (!m_inviteQueued.exchange(true)) {
        const bool queued = QueueInviteAccept();
        diag::Log("guest: native invite accept {}", queued ? "queued" : "FAILED (UI context unavailable)");
    }
}

bool Guest::QueueInviteAccept() {
    void* stateMachine = ReadPointer(GameAddress(rva::kUIStateMachineRoot + rva::kUIStateMachineOffset));
    if (stateMachine == nullptr) return false;
    void* uiContext = FieldPointer<void>(stateMachine, rva::kUIContextOffset);
    if (!IsReadable(uiContext, 0x3c0)) return false;
    diag::Log("guest: dispatching invite accept to UI context {}", uiContext);

    // Same arguments the Steam build uses for an accepted invite: lobby "0" and
    // empty details. The handler queues the join request into the UI flow.
    GameString lobbyId = {1u | kStaticStringFlag, 0, "0"};
    GameString details[2] = {{kStaticStringFlag, 0, ""}, {kStaticStringFlag, 0, ""}};
    using InviteHandler = void(__fastcall*)(void*, GameString*, GameString*);
    GameFunction<InviteHandler>(rva::kInviteHandler)(uiContext, &lobbyId, details);
    return true;
}

bool Guest::TakeJoinRequest(void* idleState, int& request) {
    if (!m_ready.load(std::memory_order_acquire) || m_joinIssued.exchange(true)) return false;
    const int previous = request;
    request = lobby::kJoin;
    if (RegisterProvider(lobby::Context(idleState))) {
        diag::Log("guest: lobby request {} -> join (4); redirecting to ZLobbyConnected", previous);
        return true;
    }
    diag::Log("guest: could not register lobby-data provider on context {}", lobby::Context(idleState));
    m_joinIssued.store(false);
    request = previous;
    return false;
}

bool Guest::RegisterProvider(void* lobbyContext) {
    if (lobbyContext == nullptr || !IsReadable(FieldPointer<void>(lobbyContext, kLobbyDataProvidersOffset), 24)) return false;
    // Register once per lobby context; the context lives for the whole game session.
    if (m_providerContext.exchange(lobbyContext) == lobbyContext) return true;
    diag::Log("guest: registering lobby-data provider on context {}", lobbyContext);
    s_instance = this;
    lobby::Delegate provider{reinterpret_cast<uintptr_t>(&ProvideLobbyData), 0, 0, 1};
    GameFunction<lobby::DelegateListFn>(rva::kDelegateAdd)(
        FieldPointer<void>(lobbyContext, kLobbyDataProvidersOffset), &provider);
    return true;
}

void __fastcall Guest::ProvideLobbyData(void*, GameString* key, GameString* out) {
    Guest* guest = s_instance;
    if (guest == nullptr || out == nullptr) return;
    std::string name;
    if (!ReadGameString(key, name)) return;
    if (!guest->m_ready.load(std::memory_order_acquire)) {
        // Only interesting while a join is in progress; otherwise this PC may be hosting.
        if (guest->Requested()) LogProviderAnswer(name, "not answered (metadata not ready)", {});
        return;
    }
    std::lock_guard lock(guest->m_dataMutex);
    const auto found = guest->m_lobbyData.find(name);
    if (found == guest->m_lobbyData.end()) {
        LogProviderAnswer(name, "not answered (unknown key)", {});
        return;
    }
    LogProviderAnswer(name, "answered", found->second);
    // Return a pointer into our map: the native join routine can keep the
    // HostAddress string while it queries HostPublicKey.
    out->length = static_cast<uint32_t>(found->second.size()) | kStaticStringFlag;
    out->padding = 0;
    out->chars = found->second.c_str();
}

void Guest::OnLobbyConnectedEnter(void* state) {
    const int request = lobby::ReadRequest(state);
    diag::Log("lobby: ZLobbyConnected enter {} (request {}, guest join {})", state, request,
        Requested() ? "active" : "inactive");
    if (Requested() && request == lobby::kJoin)
        m_lobbyState.store(state, std::memory_order_release);
}

void Guest::OnLobbyConnectedExit(void* state) {
    diag::Log("lobby: ZLobbyConnected exit {} (request {})", state, lobby::ReadRequest(state));
    void* expected = state;
    m_lobbyState.compare_exchange_strong(expected, nullptr);
    EndSession(false);
}

void Guest::OnMultiplayerIdleEnter() {
    diag::Log("multiplayer: ZMultiplayerIdle enter (invite queued {}, metadata ready {})",
        m_inviteQueued.load(), m_ready.load());
    // The multiplayer flow also passes through Idle while the join is set up;
    // only a return after the invite was queued means the player left.
    if (m_inviteQueued.load(std::memory_order_acquire) && m_ready.load(std::memory_order_acquire))
        EndSession(true);
}

void Guest::EndSession(bool requestNativeLeave) {
    if (!Requested() && !m_ready.load(std::memory_order_acquire)) return;
    diag::Log("guest: session ended ({})", requestNativeLeave ? "left multiplayer" : "lobby exited");
    if (requestNativeLeave) {
        // Ask our ZLobbyConnected to leave; its update (LocalHost's) then takes
        // the native Idle transition, whose OnExit disconnects RakNet cleanly.
        if (int* request = lobby::RequestField(m_lobbyState.load(std::memory_order_acquire))) {
            *request = lobby::kLeave;
        } else if (void* network = ReadPointer(GameAddress(rva::kNetworkStatePointer));
                   network != nullptr && IsReadable(FieldPointer<void>(network, 0xa0), 4)) {
            *FieldPointer<uint8_t>(network, 0xa0) = 0;
            *FieldPointer<int>(network, 0x20) = kNetworkStateDisconnect;
        }
    }
    // Clearing these stops Tick/TakeJoinRequest from re-entering the join.
    m_requested.store(false, std::memory_order_release);
    m_joinIssued.store(false, std::memory_order_release);
    m_inviteQueued.store(false, std::memory_order_release);
    m_fetcher.Stop();
    m_ready.store(false, std::memory_order_release);
    m_roomClose.Schedule();
    { std::lock_guard lock(m_dataMutex); m_lobbyData.clear(); }
}

void Guest::Stop() {
    m_fetcher.Stop();
    m_ready.store(false);
    m_requested.store(false);
}

} // namespace ghost::epic
