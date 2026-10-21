// Steam build: hosting and joining cross-store lobbies, and the plugin object.
//
// Hosting: "create lobby" (Steam) is rewritten to "local host" (plain RakNet).
// Lobby-data writes and the host's public key are published to the host proxy,
// and the room relay carries the guest's RakNet traffic to it.
//
// Joining: after fetching the host's metadata through the room tunnel, the
// Steam lobby shim (steam_lobby_shim.h) fakes Steam lobby 0 and the plugin
// dispatches a native "accept invite" for it. The game then runs its normal
// Steam join flow against the fake lobby.

#include "IModSDK.h"
#include "IPluginInterface.h"
#include "Glacier/ZString.h"

#include "backend/backend_patch.h"
#include "common/memory.h"
#include "common/native_lobby.h"
#include "net/deferred_room_close.h"
#include "net/host_proxy.h"
#include "net/metadata_fetcher.h"
#include "net/room_client.h"
#include "plugin_entry.h"
#include "steam/steam_lobby_shim.h"
#include "steam/steam_offsets.h"
#include "ui/room_panel.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace ghost::steam {
namespace {

using VoidMethod = void(__fastcall*)(void*);
using UpdateMethod = void*(__fastcall*)(void*, void*);
using RunCallbacks = void(__cdecl*)();

constexpr uint16_t kHostProxyPort = 30001;
constexpr const char* kGameVersion = "3.280.0.0";

rooms::Client g_room;
DeferredRoomClose g_roomClose;
net::MetadataFetcher g_metadata;
std::atomic<bool> g_hooksInstalled{false};

// Status line shown in the UI.
std::mutex g_statusMutex;
std::string g_status = "not checked";

void SetStatus(const std::string& message) {
    std::lock_guard lock(g_statusMutex);
    g_status = message;
}

std::string GetStatus() {
    std::lock_guard lock(g_statusMutex);
    return g_status;
}

// Host state.
std::atomic<bool> g_hostActive{false};      // a LocalHost lobby is running
std::atomic<bool> g_hostLobbyReady{false};  // the lobby reached Connected
std::string g_hostKey;                      // published once captured

// Guest state. g_joinRequested/g_requestedCode come from the UI thread; the
// rest is only touched on the game thread (SteamAPI_RunCallbacks).
std::atomic<bool> g_joinBusy{false};
std::atomic<bool> g_joinRequested{false};
std::mutex g_codeMutex;
std::string g_requestedCode;
bool g_waitingForRoom = false;
std::string g_tunnelEndpoint;
void* g_pendingLobbyEnter = nullptr;        // ZLobbyJoin waiting for LobbyEnter_t

struct Hooks {
    PointerHook lobbyIdleEnter;
    PointerHook lobbyIdleUpdate;
    PointerHook lobbyJoinEnter;
    PointerHook lobbyJoinExit;
    PointerHook lobbyConnectedEnter;
    PointerHook localHostEnter;
    PointerHook localHostExit;
    PointerHook openInviteDialog;
    PointerHook runCallbacks;

    std::vector<PointerHook*> All() {
        return {&lobbyIdleEnter, &lobbyIdleUpdate, &lobbyJoinEnter, &lobbyJoinExit, &lobbyConnectedEnter,
            &localHostEnter, &localHostExit, &openInviteDialog, &runCallbacks};
    }
} g_hooks;

// --- Shared ---------------------------------------------------------------------

// Returns to the idle state: closes the room, the fake lobby and any fetch.
void ResetSession() {
    g_roomClose.Clear();
    g_room.Cancel();
    g_waitingForRoom = false;
    g_pendingLobbyEnter = nullptr;
    shim::Deactivate();
    g_hostActive.store(false, std::memory_order_release);
    g_hostLobbyReady.store(false, std::memory_order_release);
    g_hostKey.clear();
    g_metadata.Stop();
    g_joinBusy.store(false, std::memory_order_release);
}

// --- Hosting ----------------------------------------------------------------------

bool IsGameCode(const void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    return address >= GameBase() + rva::kCodeStart && address < GameBase() + rva::kCodeEnd;
}

// Reads the host's RakNet public key from the network context's key provider
// (+0xb0) and publishes it as HostPublicKey.
bool PublishKeyFromProvider(void* networkContext) {
    void* provider = networkContext != nullptr ? ReadPointer(FieldPointer<void>(networkContext, 0xb0)) : nullptr;
    void** vtable = provider != nullptr ? static_cast<void**>(ReadPointer(provider)) : nullptr;
    if (vtable == nullptr || !IsReadable(vtable + 2, 2 * sizeof(void*))) return false;
    if (!IsGameCode(vtable[2]) || !IsGameCode(vtable[3])) return false;

    using HasKey = bool(__fastcall*)(void*);
    using GetKey = void*(__fastcall*)(void*);
    using KeyToString = void(__fastcall*)(void*, void*);
    if (!reinterpret_cast<HasKey>(vtable[2])(provider)) return false;
    void* key = reinterpret_cast<GetKey>(vtable[3])(provider);
    if (key == nullptr || !IsReadable(key, 24)) return false;

    alignas(16) GameString text = {};
    GameFunction<KeyToString>(rva::kKeyToString)(&text, key);
    const uint32_t length = text.length & 0x3fffffffu;
    if (text.chars == nullptr || length == 0 || length > 4096 || !IsReadable(text.chars, length)) return false;
    g_hostKey.assign(text.chars, length);
    if (g_hostKey.size() > metadata::kMaxValue) {
        g_hostKey.clear();
        return false;
    }
    PublishHostLobbyField("HostPublicKey", g_hostKey);
    return true;
}

// Retries every 500 ms until the key is available.
void TryPublishHostKey() {
    if (!g_hostActive.load(std::memory_order_acquire) || !g_hostKey.empty()) return;
    static uint64_t lastAttempt = 0;
    const uint64_t now = GetTickCount64();
    if (lastAttempt != 0 && now - lastAttempt < 500) return;
    lastAttempt = now;
    // Only the network manager owns the connected key provider; don't probe
    // the lobby state's +0xb0 before the network state exists.
    PublishKeyFromProvider(ReadPointer(GameAddress(rva::kNetworkManager + 0x20)));
}

void __fastcall ObserveHostLobbyWrite(void*, void* key, void* value) {
    if (!g_hostActive.load(std::memory_order_acquire)) return;
    std::string keyText, valueText;
    if (ReadGameString(key, keyText) && ReadGameString(value, valueText)) PublishHostLobbyField(keyText, valueText);
}

void ObserveHostLobby(void* state) {
    void* context = lobby::Context(state);
    void* delegates = context != nullptr ? FieldPointer<void>(context, lobby::kLobbyDataDelegatesOffset) : nullptr;
    if (delegates == nullptr || !IsReadable(delegates, 24)) return;
    lobby::Delegate observer{reinterpret_cast<uintptr_t>(&ObserveHostLobbyWrite), 0, 0, reinterpret_cast<uintptr_t>(state)};
    GameFunction<lobby::DelegateListFn>(rva::kDelegateAdd)(delegates, &observer);
}

void __fastcall LocalHostEnterDetour(void* self) {
    g_roomClose.Clear();
    g_room.Cancel();
    g_hostKey.clear();
    BeginHostLobby();
    ResetHostProxyPeer();
    PublishHostLobbyField("HostGameVersion", kGameVersion);
    g_hooks.localHostEnter.Original<VoidMethod>()(self);
    g_hostActive.store(true, std::memory_order_release);
    ObserveHostLobby(self);
    SetStatus("Steam host lobby created; waiting for map metadata");
}

void __fastcall LocalHostExitDetour(void* self) {
    g_hostActive.store(false, std::memory_order_release);
    EndHostLobby();
    g_roomClose.Schedule();
    g_hooks.localHostExit.Original<VoidMethod>()(self);
    SetStatus("Steam host lobby closed");
}

// Room codes replace Steam invites, and a local-host lobby has no Steam lobby
// to invite to, so never open the overlay invite dialog.
void __fastcall OpenInviteDialogDetour(void*) {}

// --- Joining ----------------------------------------------------------------------

// The UI's invite handler is registered once the Ghost Mode menu has been opened.
bool InviteHandlerReady() {
    const uintptr_t list = GameBase() + rva::kInviteDelegates;
    if (!IsReadable(reinterpret_cast<void*>(list), 32)) return false;
    const uint64_t flags = *reinterpret_cast<uint64_t*>(list + 16);
    const uintptr_t begin = *reinterpret_cast<uintptr_t*>(list);
    const uintptr_t end = *reinterpret_cast<uintptr_t*>(list + 8);
    // Small delegate lists are stored inline (flag bit 62, count in the low byte).
    const bool inlined = (flags & (uint64_t(1) << 62)) != 0;
    if (!inlined && (end < begin || end - begin > 32 * 64)) return false;
    const size_t count = inlined ? flags & 0xff : (end - begin) / 32;
    if (count == 0 || count > 64 || (inlined && count > 1)) return false;
    const uintptr_t entries = inlined ? list : begin;
    if (!IsReadable(reinterpret_cast<void*>(entries), count * 32)) return false;
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t entry = entries + i * 32;
        if (*reinterpret_cast<uintptr_t*>(entry) == GameBase() + rva::kUIInviteHandler &&
            *reinterpret_cast<uintptr_t*>(entry + 24) != 0) return true;
    }
    return false;
}

// Accepts a native invite to the shim's lobby 0. The dispatcher ignores its
// first argument and forwards the lobby id and (empty) details to the UI.
void DispatchInvite() {
    ZString lobbyId(std::string_view("0"));
    ZString details[2] = {ZString(std::string_view("")), ZString(std::string_view(""))};
    using Dispatch = void(__fastcall*)(void*, ZString*, ZString*);
    GameFunction<Dispatch>(rva::kInviteDispatcher)(nullptr, &lobbyId, details);
    SetStatus("Native invite queued: lobby=0; awaiting UI prerequisites");
}

void StartMetadataFetch() {
    if (!InviteHandlerReady()) {
        SetStatus("Join unavailable: native UI invite handler is not registered");
        ResetSession();
        return;
    }
    std::string error;
    if (!g_metadata.Start(g_tunnelEndpoint, error)) {
        SetStatus("Metadata: " + error);
        ResetSession();
        return;
    }
    SetStatus("Fetching current lobby metadata from " + g_tunnelEndpoint);
}

void PollMetadata() {
    metadata::Fields fields;
    switch (g_metadata.Poll(fields)) {
        case net::MetadataFetcher::Progress::Pending:
            return;
        case net::MetadataFetcher::Progress::WaitingChanged:
            SetStatus("Metadata waiting: " + g_metadata.Waiting());
            return;
        case net::MetadataFetcher::Progress::TimedOut:
            SetStatus("Metadata timed out: " +
                (g_metadata.Waiting().empty() ? std::string("host lobby is not responding") : g_metadata.Waiting()));
            ResetSession();
            return;
        case net::MetadataFetcher::Progress::Ready:
            break;
    }
    // The host's own address is not reachable; the game must use the tunnel.
    fields["HostAddress"] = g_tunnelEndpoint;
    if (!shim::Install() || !shim::OpenRelay(g_metadata.Endpoint())) {
        SetStatus("Join setup failed: Steam identity/interfaces or relay socket unavailable");
        ResetSession();
        return;
    }
    shim::Activate(std::move(fields));
    DispatchInvite();
}

// Delivers Steam's LobbyEnter_t (success) for lobby 0 to ZLobbyJoin. Steam
// itself never sends it because the lobby does not exist.
void DeliverPendingLobbyEnter() {
    void* state = g_pendingLobbyEnter;
    if (state == nullptr) return;
    g_pendingLobbyEnter = nullptr;
    // ZLobbyJoin::OnExit clears the pointer if the state left before this tick.
    if (!shim::Active() || !IsReadable(state, 8) || *static_cast<uintptr_t*>(state) != GameBase() + rva::kLobbyJoinVtable)
        return;
    alignas(8) uint8_t lobbyEnter[24] = {};
    *reinterpret_cast<uint64_t*>(lobbyEnter) = shim::kLobbyId;
    *reinterpret_cast<uint32_t*>(lobbyEnter + 16) = 1;   // k_EChatRoomEnterResponseSuccess
    GameFunction<void(__fastcall*)(void*, void*)>(rva::kLobbyJoinOnLobbyEnter)(state, lobbyEnter);
}

void TickJoin() {
    if (g_joinRequested.exchange(false, std::memory_order_acq_rel)) {
        std::string code;
        { std::lock_guard lock(g_codeMutex); code = g_requestedCode; }
        if (!InviteHandlerReady()) {
            SetStatus("Open the Ghost Mode menu before joining");
            ResetSession();
        } else if (!g_room.Start(false, code, kHostProxyPort, {}, "steam")) {
            SetStatus("Previous room is closing; try again shortly");
            ResetSession();
        } else {
            g_waitingForRoom = true;
            SetStatus("Connecting by room code...");
        }
    }
    if (g_waitingForRoom) {
        const auto room = g_room.Get();
        if (room.state == rooms::State::Ready) {
            g_waitingForRoom = false;
            g_tunnelEndpoint = room.localEndpoint;
            SetStatus("Room connected; fetching host lobby");
            StartMetadataFetch();
        } else if (room.state == rooms::State::Failed || room.state == rooms::State::Idle) {
            SetStatus(room.status);
            ResetSession();
        }
    }
    PollMetadata();
    DeliverPendingLobbyEnter();
}

// --- Game hooks ---------------------------------------------------------------------

void __cdecl RunCallbacksDetour() {
    g_hooks.runCallbacks.Original<RunCallbacks>()();
    g_roomClose.Tick(g_room);
    TryPublishHostKey();
    TickJoin();
}

void __fastcall LobbyIdleEnterDetour(void* self) {
    const bool joined = shim::Active();
    const bool hosted = g_hostActive.load();
    if (joined || hosted) ResetSession();
    g_hooks.lobbyIdleEnter.Original<VoidMethod>()(self);
    if (joined) SetStatus("Native lobby returned to Idle; ready to join again");
    if (hosted) SetStatus("Host lobby closed; ready again");
}

void* __fastcall LobbyIdleUpdateDetour(void* self, void* transition) {
    if (!g_hostActive.load() && !shim::Active()) {
        if (int* request = lobby::RequestField(self); request != nullptr && *request == lobby::kCreate)
            *request = lobby::kLocalHost;
    }
    return g_hooks.lobbyIdleUpdate.Original<UpdateMethod>()(self, transition);
}

void __fastcall LobbyJoinEnterDetour(void* self) {
    g_hooks.lobbyJoinEnter.Original<VoidMethod>()(self);
    if (shim::Active()) g_pendingLobbyEnter = self;
}

void __fastcall LobbyJoinExitDetour(void* self) {
    if (g_pendingLobbyEnter == self) g_pendingLobbyEnter = nullptr;
    g_hooks.lobbyJoinExit.Original<VoidMethod>()(self);
}

void __fastcall LobbyConnectedEnterDetour(void* self) {
    g_hooks.lobbyConnectedEnter.Original<VoidMethod>()(self);
    if (g_hostActive.load()) {
        TryPublishHostKey();
        SetHostLobbyTransportReady();
        g_hostLobbyReady.store(true, std::memory_order_release);
        SetStatus(IsHostLobbyReady() ? "Host lobby ready; press Host to create the room"
                                     : "Host lobby connected; waiting for map/public-key metadata");
    } else if (shim::Active()) {
        SetStatus("Native lobby connected; waiting for session/UI population");
    }
}

bool InstallHooks() {
    struct Spec { PointerHook& hook; uintptr_t vtable; size_t slot; uintptr_t expected; void* detour; };
    const Spec specs[] = {
        {g_hooks.lobbyIdleEnter, rva::kLobbyIdleVtable, 5, rva::kLobbyIdleEnter, &LobbyIdleEnterDetour},
        {g_hooks.lobbyIdleUpdate, rva::kLobbyIdleVtable, 10, rva::kLobbyIdleUpdate, &LobbyIdleUpdateDetour},
        {g_hooks.lobbyJoinEnter, rva::kLobbyJoinVtable, 5, rva::kLobbyJoinEnter, &LobbyJoinEnterDetour},
        {g_hooks.lobbyJoinExit, rva::kLobbyJoinVtable, 6, rva::kLobbyJoinExit, &LobbyJoinExitDetour},
        {g_hooks.lobbyConnectedEnter, rva::kLobbyConnectedVtable, 5, rva::kLobbyConnectedEnter, &LobbyConnectedEnterDetour},
        {g_hooks.localHostEnter, rva::kLocalHostVtable, 5, rva::kLocalHostEnter, &LocalHostEnterDetour},
        {g_hooks.localHostExit, rva::kLocalHostVtable, 6, rva::kLocalHostExit, &LocalHostExitDetour},
        {g_hooks.openInviteDialog, rva::kSteamLobbyManagerVtable, 12, rva::kOpenInviteDialog, &OpenInviteDialogDetour},
    };
    bool ok = true;
    for (const Spec& spec : specs) ok = ok && HookVtableSlot(spec.hook, spec.vtable, spec.slot, spec.expected, spec.detour);

    // The game thread calls SteamAPI_RunCallbacks every frame; it drives our tick.
    HMODULE steamApi = GetModuleHandleW(L"steam_api64.dll");
    void* runCallbacks = steamApi ? reinterpret_cast<void*>(GetProcAddress(steamApi, "SteamAPI_RunCallbacks")) : nullptr;
    ok = ok && runCallbacks != nullptr &&
        g_hooks.runCallbacks.Install(GameAddress<void*>(rva::kSteamApiRunCallbacksImport),
            reinterpret_cast<void*>(&RunCallbacksDetour), runCallbacks);

    if (!ok) for (PointerHook* hook : g_hooks.All()) hook->Restore();
    g_hooksInstalled.store(ok, std::memory_order_release);
    SetStatus(ok ? "Native invite hooks installed; enter the Ghost Mode menu to join"
                 : "Hook installation failed; executable signatures did not match");
    return ok;
}

class SteamPlugin final : public IPluginInterface {
public:
    ~SteamPlugin() override {
        for (PointerHook* hook : g_hooks.All()) hook->Restore();
        shim::Uninstall();
        ResetSession();
        g_room.Stop();
    }

    void Init() override {
        m_backendReady = m_backend.Apply(Storefront::Steam);
        // Steam's local-host lobby uses RakNet on UDP 30000, which the room
        // client reaches through this loopback proxy when hosting.
        StartHostProxy(kHostProxyPort);
    }

    void OnEngineInitialized() override {
        m_backendReady = m_backend.Apply(Storefront::Steam);
        if (!g_hooksInstalled.load()) InstallHooks();
    }

    void OnDrawMenu() override {
        const bool hooks = g_hooksInstalled.load();
        ui::RoomPanelState panel{"Steam — host or join a cross-store lobby"};
        panel.notice = GetStatus();
        panel.backendStatus = m_backend.Status();
        panel.room = g_room.Get();
        const bool busy = g_joinBusy.load(std::memory_order_acquire) || rooms::IsBusy(panel.room.state);
        panel.canHost = !busy && m_backendReady && hooks && g_hostLobbyReady.load() && IsHostLobbyReady();
        panel.canJoin = !busy && m_backendReady && hooks;
        panel.showClose = busy;

        std::string code;
        switch (ui::DrawRoomPanel(panel, code)) {
            case ui::RoomPanelAction::Host:
                SetStatus(g_room.Start(true, {}, kHostProxyPort, {}, "steam")
                    ? "Creating Steam host room..." : "Previous room is closing; try again shortly");
                break;
            case ui::RoomPanelAction::Join:
                { std::lock_guard lock(g_codeMutex); g_requestedCode = code; }
                g_joinBusy.store(true, std::memory_order_release);
                g_joinRequested.store(true, std::memory_order_release);
                break;
            case ui::RoomPanelAction::Close:
                g_room.Cancel();
                g_joinBusy.store(false);
                break;
            case ui::RoomPanelAction::None:
                break;
        }
    }

private:
    BackendPatch m_backend;
    std::atomic<bool> m_backendReady{false};
};

} // namespace

IPluginInterface* CreatePlugin() { return new SteamPlugin(); }

} // namespace ghost::steam
