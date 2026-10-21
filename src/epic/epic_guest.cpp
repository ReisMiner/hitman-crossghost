#include "epic/epic_guest.h"

#include "common/native_lobby.h"
#include "epic/epic_offsets.h"

namespace ghost::epic {
namespace {

// The lobby context's list of lobby-data providers (queried by key).
constexpr size_t kLobbyDataProvidersOffset = 0x170;

// Network state value the game writes when leaving a lobby (ZLobbyConnected::OnExit).
constexpr int kNetworkStateDisconnect = 7;

// Steam hosts may only publish a preset. The Epic join routine resolves the map
// through ContractId, so fill it in for the two supported Ghost Mode presets.
void FillContractFromPreset(metadata::Fields& fields) {
    if (!fields["ContractId"].empty()) return;
    const std::string& preset = fields["PresetId"];
    if (preset == "d72d7cc9-ee26-4c7d-857a-75abdc9ccb61")
        fields["ContractId"] = "c854d9c7-8991-42db-a2e4-3cf93e645e81";
    else if (preset == "d5ad70f4-7dbb-4b2b-b183-061778a51468")
        fields["ContractId"] = "d5ad70f4-7dbb-4b2b-b183-061778a51468";
}

} // namespace

Guest* Guest::s_instance = nullptr;

bool Guest::Join(const std::string& code) {
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
    m_fetcher.Start(m_endpoint, error);
}

void Guest::PollMetadata() {
    metadata::Fields fields;
    switch (m_fetcher.Poll(fields)) {
        case net::MetadataFetcher::Progress::TimedOut:
            // Retried on the next tick while the room is still up.
            m_ready.store(false, std::memory_order_release);
            return;
        case net::MetadataFetcher::Progress::Ready:
            break;
        default:
            return;
    }
    // The native Joining state uses both the IP and the port. Point it at the
    // tunnel so incoming and outgoing RakNet addresses agree.
    fields["HostAddress"] = m_endpoint;
    FillContractFromPreset(fields);
    { std::lock_guard lock(m_dataMutex); m_lobbyData = std::move(fields); }
    m_ready.store(true, std::memory_order_release);
    if (!m_inviteQueued.exchange(true)) QueueInviteAccept();
}

bool Guest::QueueInviteAccept() {
    void* stateMachine = ReadPointer(GameAddress(rva::kUIStateMachineRoot + rva::kUIStateMachineOffset));
    if (stateMachine == nullptr) return false;
    void* uiContext = FieldPointer<void>(stateMachine, rva::kUIContextOffset);
    if (!IsReadable(uiContext, 0x3c0)) return false;

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
    if (RegisterProvider(lobby::Context(idleState))) return true;
    m_joinIssued.store(false);
    request = previous;
    return false;
}

bool Guest::RegisterProvider(void* lobbyContext) {
    if (lobbyContext == nullptr || !IsReadable(FieldPointer<void>(lobbyContext, kLobbyDataProvidersOffset), 24)) return false;
    // Register once per lobby context; the context lives for the whole game session.
    if (m_providerContext.exchange(lobbyContext) == lobbyContext) return true;
    s_instance = this;
    lobby::Delegate provider{reinterpret_cast<uintptr_t>(&ProvideLobbyData), 0, 0, 1};
    GameFunction<lobby::DelegateListFn>(rva::kDelegateAdd)(
        FieldPointer<void>(lobbyContext, kLobbyDataProvidersOffset), &provider);
    return true;
}

void __fastcall Guest::ProvideLobbyData(void*, GameString* key, GameString* out) {
    Guest* guest = s_instance;
    if (guest == nullptr || out == nullptr || !guest->m_ready.load(std::memory_order_acquire)) return;
    std::string name;
    if (!ReadGameString(key, name)) return;
    std::lock_guard lock(guest->m_dataMutex);
    const auto found = guest->m_lobbyData.find(name);
    if (found == guest->m_lobbyData.end()) return;
    // Return a pointer into our map: the native join routine can keep the
    // HostAddress string while it queries HostPublicKey.
    out->length = static_cast<uint32_t>(found->second.size()) | kStaticStringFlag;
    out->padding = 0;
    out->chars = found->second.c_str();
}

void Guest::OnLobbyConnectedEnter(void* state) {
    if (Requested() && lobby::ReadRequest(state) == lobby::kJoin)
        m_lobbyState.store(state, std::memory_order_release);
}

void Guest::OnLobbyConnectedExit(void* state) {
    void* expected = state;
    m_lobbyState.compare_exchange_strong(expected, nullptr);
    EndSession(false);
}

void Guest::OnMultiplayerIdleEnter() {
    // The multiplayer flow also passes through Idle while the join is set up;
    // only a return after the invite was queued means the player left.
    if (m_inviteQueued.load(std::memory_order_acquire) && m_ready.load(std::memory_order_acquire))
        EndSession(true);
}

void Guest::EndSession(bool requestNativeLeave) {
    if (!Requested() && !m_ready.load(std::memory_order_acquire)) return;
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
