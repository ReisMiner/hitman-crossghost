// Epic build: hosting a cross-store lobby and the plugin entry object.
// Joining is implemented in epic_guest.cpp.
//
// Hosting overview:
//  - ZLobbyIdle: "create lobby" (EOS) is rewritten to "local host" (plain
//    RakNet), which a Steam player can reach.
//  - ZLobbyCreateLocalHost: lobby-data writes are observed and published to the
//    host proxy so the guest can fetch map/key/version.
//  - Once the network state is Connected, the EOS socket override is removed so
//    RakNet sends real UDP, which the host proxy and room relay carry.

#include "IModSDK.h"
#include "IPluginInterface.h"

#include "backend/backend_patch.h"
#include "common/memory.h"
#include "common/native_lobby.h"
#include "epic/epic_guest.h"
#include "epic/epic_offsets.h"
#include "net/deferred_room_close.h"
#include "net/host_proxy.h"
#include "net/room_client.h"
#include "plugin_entry.h"
#include "ui/room_panel.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace ghost::epic {
namespace {

using VoidMethod = void(__fastcall*)(void*);
using UpdateMethod = void*(__fastcall*)(void*, void*);
// PluginInterface2::OnNewConnection(const SystemAddress&, RakNetGUID, bool isIncoming);
// the GUID is passed by pointer to a copy.
using NewConnectionCallback = void(__fastcall*)(void*, void*, void*, bool);
using RakPeerStartup = int(__fastcall*)(void*, unsigned short, void*, unsigned int, int);

constexpr uint16_t kHostProxyPort = 30001;

rooms::Client g_room;
DeferredRoomClose g_roomClose;
Guest g_guest(g_room, g_roomClose);

std::string g_gameVersion;                   // e.g. "3.290.1.0", sent as HostGameVersion
std::atomic<bool> g_hostLobbyActive{false};  // a LocalHost lobby is running
void* g_observedHostState = nullptr;
std::atomic<void*> g_rakPeer{nullptr};       // RakPeer after a successful Startup
std::atomic<bool> g_hooksInstalled{false};
// Last reason EnableNativeUdp stopped early, shown while the host waits for transport.
std::atomic<const char*> g_transportIssue{"RakNet has not connected a lobby yet"};

// The guest's replication connection to the host, deferred until the session
// is active (see ReplicaNewConnectionDetour). Game thread only.
struct DeferredReplicaConnection {
    bool pending = false;
    void* manager = nullptr;
    alignas(8) uint8_t address[20] = {};   // RakNet::SystemAddress (IPv4 build: sockaddr_in + port + index)
    alignas(8) uint8_t guid[16] = {};      // RakNet::RakNetGUID
    bool incoming = false;
    uint64_t activeSince = 0;              // GetTickCount64 when the session became active
} g_guestReplica;

struct Hooks {
    PointerHook lobbyIdleUpdate;
    PointerHook localHostEnter;
    PointerHook localHostExit;
    PointerHook connectedEnter;
    PointerHook connectedExit;
    PointerHook connectedUpdate;
    PointerHook multiplayerIdleEnter;
    PointerHook rakPeerStartup;
    PointerHook networkConnectingEnter;
    PointerHook networkConnectedEnter;
    PointerHook replicaNewConnection;

    std::vector<PointerHook*> All() {
        return {&lobbyIdleUpdate, &localHostEnter, &localHostExit, &connectedEnter, &connectedExit,
            &connectedUpdate, &multiplayerIdleEnter, &rakPeerStartup, &networkConnectingEnter,
            &networkConnectedEnter, &replicaNewConnection};
    }
} g_hooks;

// --- Game patches -------------------------------------------------------------

// The host's NAT check fails on a RakNet local-host lobby; skip it and its popup.
void DisableHostNatWarning() {
    const uint8_t returnTrue[] = {0xb0, 0x01, 0xc3};   // mov al, 1; ret
    const uint8_t returnVoid[] = {0xc3};               // ret
    WriteCode(GameAddress(rva::kHostNatCheck), returnTrue, sizeof(returnTrue));
    WriteCode(GameAddress(rva::kNatWarningPopup), returnVoid, sizeof(returnVoid));
}

int __fastcall EosGetNatTypeDetour(void*, int* natType) {
    if (natType != nullptr) *natType = 1;   // EOS_NAT_Open
    return 0;
}

int __fastcall EosQueryNatTypeDetour(void* handle, const void*, void*, void(__fastcall* callback)(const void*)) {
    struct QueryNatTypeInfo {
        int result;
        int padding;
        void* p2pHandle;
        int natType;
        int padding2;
    } info = {0, 0, handle, 1, 0};
    if (callback != nullptr) callback(&info);
    return 0;
}

// EOS always reports an open NAT, so the game does not warn or refuse.
void ReportOpenNatToEos() {
    static std::atomic<bool> done{false};
    if (done.exchange(true, std::memory_order_acq_rel)) return;
    HMODULE eos = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    if (eos == nullptr) {
        done.store(false, std::memory_order_release);   // retry once EOS is loaded
        return;
    }
    RedirectFunction(reinterpret_cast<void*>(GetProcAddress(eos, "EOS_P2P_GetNATType")),
        reinterpret_cast<void*>(&EosGetNatTypeDetour));
    RedirectFunction(reinterpret_cast<void*>(GetProcAddress(eos, "EOS_P2P_QueryNATType")),
        reinterpret_cast<void*>(&EosQueryNatTypeDetour));
}

std::string ReadGameVersion() {
    wchar_t executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    const DWORD size = GetFileVersionInfoSizeW(executable, nullptr);
    std::vector<uint8_t> bytes(size);
    VS_FIXEDFILEINFO* version = nullptr;
    UINT fixedSize = 0;
    if (!size || !GetFileVersionInfoW(executable, 0, size, bytes.data()) ||
        !VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&version), &fixedSize) ||
        fixedSize < sizeof(*version) || version->dwSignature != 0xfeef04bd) return {};
    return std::to_string(HIWORD(version->dwFileVersionMS)) + "." + std::to_string(LOWORD(version->dwFileVersionMS)) +
        "." + std::to_string(HIWORD(version->dwFileVersionLS)) + "." + std::to_string(LOWORD(version->dwFileVersionLS));
}

// --- Native transport ---------------------------------------------------------

// The Epic build routes RakNet through an "EpicSocketOverride" whose Send is a
// stub that reports success without sending. Removing the override makes
// RakNet fall back to its real UDP socket. Reapplied for every lobby, because
// each lobby restarts RakNet with a new socket.
void EnableNativeUdp(bool hosting) {
    void* rakPeer = g_rakPeer.load(std::memory_order_acquire);
    if (rakPeer == nullptr) {
        g_transportIssue.store("RakPeer::Startup was not observed");
        return;
    }
    DisableHostNatWarning();
    ReportOpenNatToEos();

    void* sockets = ReadPointer(FieldPointer<void>(rakPeer, 0x5a8));
    void* socket = sockets != nullptr ? ReadPointer(sockets) : nullptr;
    if (!IsReadable(socket, 0x88) || *static_cast<uintptr_t*>(socket) != GameBase() + rva::kRakNetSocketVtable) {
        g_transportIssue.store("RakNet socket was not recognized");
        return;
    }

    auto** overrideSlot = FieldPointer<void*>(socket, 0x80);
    void* layerOverride = *overrideSlot;
    if (layerOverride != nullptr &&
        (!IsReadable(layerOverride, sizeof(uintptr_t)) ||
         *static_cast<uintptr_t*>(layerOverride) != GameBase() + rva::kEpicSocketOverrideVtable)) {
        g_transportIssue.store("EOS socket override was not recognized");
        return;
    }
    InterlockedExchangePointer(overrideSlot, nullptr);
    g_transportIssue.store("");
    if (hosting) {
        ResetHostProxyPeer();
        SetHostLobbyTransportReady();
    }
}

int __fastcall RakPeerStartupDetour(void* self, unsigned short maxConnections, void* descriptors,
    unsigned int descriptorCount, int threadPriority) {
    const int result = g_hooks.rakPeerStartup.Original<RakPeerStartup>()(
        self, maxConnections, descriptors, descriptorCount, threadPriority);
    // RakNet's StartupResult enum: 0 is RAKNET_STARTED.
    g_rakPeer.store(result == 0 ? self : nullptr, std::memory_order_release);
    return result;
}

void __fastcall NetworkConnectingEnterDetour(void* self) {
    g_hooks.networkConnectingEnter.Original<VoidMethod>()(self);
    // The guest must have real UDP before Joining starts connecting.
    if (g_guest.Requested()) EnableNativeUdp(false);
}

void __fastcall NetworkConnectedEnterDetour(void* self) {
    g_hooks.networkConnectedEnter.Original<VoidMethod>()(self);
    EnableNativeUdp(!g_guest.Requested());
}

// Sets the replica manager's per-connection enable flag (+0x38) before the
// native handler runs.
bool SessionActive() {
    void* net = ReadPointer(GameAddress(rva::kNetworkObject));
    return net != nullptr && IsReadable(FieldPointer<void>(net, rva::kNetworkSessionActive), 1) &&
           *FieldPointer<uint8_t>(net, rva::kNetworkSessionActive) != 0;
}

bool HasReplicaConnection(void* manager, uint64_t guid) {
    void* world = ReadPointer(FieldPointer<void>(manager, rva::kReplicaWorld));
    if (world == nullptr || !IsReadable(world, 0x10)) return false;
    void* const* connections = static_cast<void* const*>(ReadPointer(world));
    const uint32_t count = *FieldPointer<uint32_t>(world, 0x08);
    for (uint32_t i = 0; connections != nullptr && i < count && IsReadable(connections + i, sizeof(void*)); ++i) {
        void* connection = connections[i];
        if (IsReadable(FieldPointer<void>(connection, rva::kReplicaConnectionGuid), sizeof(uint64_t)) &&
            *FieldPointer<uint64_t>(connection, rva::kReplicaConnectionGuid) == guid) return true;
    }
    return false;
}

// The Epic game only creates replication connections when the mesh join
// completes, so CrossGhost turns on the replica manager's auto-create flag
// (+0x38) for the host. The guest must not do this: its session becomes active
// only when the host accepts the mesh join, and until then the game drops every
// replica it receives (0x1005150 registers nothing while the session is
// inactive). If both sides create the connection on connect, the host sends
// its own ZNetPlayer straight away, the guest drops it, and the guest's player
// registry can never bind the host's slot: the guest sees only itself and its
// ready flag goes nowhere. A Steam peer creates its side natively, so mixed
// lobbies never hit this. The guest leaves the connection to the game and only
// creates it itself if the game has not shortly after the session is active.
void __fastcall ReplicaNewConnectionDetour(void* self, void* address, void* guid, bool incoming) {
    uint8_t* autoCreate = FieldPointer<uint8_t>(self, rva::kReplicaAutoCreate);
    if (IsReadable(autoCreate, 1)) {
        if (g_guest.Requested() && IsReadable(address, sizeof(g_guestReplica.address)) &&
            IsReadable(guid, sizeof(g_guestReplica.guid))) {
            g_guestReplica.pending = true;
            g_guestReplica.manager = self;
            std::memcpy(g_guestReplica.address, address, sizeof(g_guestReplica.address));
            std::memcpy(g_guestReplica.guid, guid, sizeof(g_guestReplica.guid));
            g_guestReplica.incoming = incoming;
            g_guestReplica.activeSince = 0;
            *autoCreate = 0;
        } else {
            *autoCreate = 1;
        }
    }
    g_hooks.replicaNewConnection.Original<NewConnectionCallback>()(self, address, guid, incoming);
}

// Guest lobby update: once the session is active, make sure the replication
// connection to the host exists. The game normally creates it itself when the
// mesh join completes; this is the fallback.
void EnsureGuestReplicaConnection() {
    DeferredReplicaConnection& deferred = g_guestReplica;
    if (!deferred.pending) return;
    uint64_t guid = 0;
    std::memcpy(&guid, deferred.guid, sizeof(guid));
    if (HasReplicaConnection(deferred.manager, guid)) {
        deferred.pending = false;
        return;
    }
    if (!SessionActive()) return;
    const uint64_t now = GetTickCount64();
    if (deferred.activeSince == 0) deferred.activeSince = now;
    constexpr uint64_t kNativeGraceMs = 2000;
    if (now - deferred.activeSince < kNativeGraceMs) return;
    uint8_t* autoCreate = FieldPointer<uint8_t>(deferred.manager, rva::kReplicaAutoCreate);
    *autoCreate = 1;
    g_hooks.replicaNewConnection.Original<NewConnectionCallback>()(
        deferred.manager, deferred.address, deferred.guid, deferred.incoming);
    *autoCreate = 0;
    deferred.pending = false;
}

// --- Hosting ------------------------------------------------------------------

void __fastcall ObserveHostLobbyWrite(void*, void* key, void* value) {
    std::string keyText, valueText;
    if (ReadGameString(key, keyText) && ReadGameString(value, valueText)) PublishHostLobbyField(keyText, valueText);
}

void SetHostLobbyObserver(void* state, bool add) {
    void* context = lobby::Context(state);
    if (context == nullptr) return;
    lobby::Delegate observer{reinterpret_cast<uintptr_t>(&ObserveHostLobbyWrite), 0, 0, reinterpret_cast<uintptr_t>(state)};
    GameFunction<lobby::DelegateListFn>(add ? rva::kDelegateAdd : rva::kDelegateRemove)(
        FieldPointer<void>(context, lobby::kLobbyDataDelegatesOffset), &observer);
    g_observedHostState = add ? state : nullptr;
}

void __fastcall LocalHostEnterDetour(void* self) {
    g_roomClose.Clear();
    g_room.Cancel();
    BeginHostLobby();
    // The local-host path skips the state that publishes the game version.
    PublishHostLobbyField("HostGameVersion", g_gameVersion);
    g_hostLobbyActive.store(lobby::Context(self) != nullptr, std::memory_order_release);
    g_hooks.localHostEnter.Original<VoidMethod>()(self);
    SetHostLobbyObserver(self, true);
}

void __fastcall LocalHostExitDetour(void* self) {
    if (g_observedHostState == self) SetHostLobbyObserver(self, false);
    g_hostLobbyActive.store(false, std::memory_order_release);
    EndHostLobby();
    g_roomClose.Schedule();
    g_hooks.localHostExit.Original<VoidMethod>()(self);
}

// --- Lobby state machine ------------------------------------------------------

void* __fastcall LobbyIdleUpdateDetour(void* state, void* transition) {
    g_roomClose.Tick(g_room);
    g_guest.Tick();
    // Back in Idle with a host lobby still marked active: it ended without exit.
    if (g_hostLobbyActive.exchange(false, std::memory_order_acq_rel)) {
        EndHostLobby();
        g_room.Cancel();
    }

    bool redirectToConnected = false;
    if (int* request = lobby::RequestField(state)) {
        redirectToConnected = g_guest.TakeJoinRequest(state, *request);
        if (!redirectToConnected && *request == lobby::kCreate) *request = lobby::kLocalHost;
    }

    void* result = g_hooks.lobbyIdleUpdate.Original<UpdateMethod>()(state, transition);
    // The guest skips the EOS join states and goes straight to Connected,
    // where the native join routine reads our lobby-data provider.
    if (redirectToConnected && transition != nullptr && IsReadable(transition, 24))
        *static_cast<uintptr_t*>(transition) = GameBase() + rva::kConnectedStateConstructor;
    return result;
}

void __fastcall LobbyConnectedEnterDetour(void* self) {
    g_guest.OnLobbyConnectedEnter(self);
    g_hooks.connectedEnter.Original<VoidMethod>()(self);
}

void __fastcall LobbyConnectedExitDetour(void* self) {
    g_hooks.connectedExit.Original<VoidMethod>()(self);
    if (g_guest.IsGuestLobby(self)) g_guestReplica.pending = false;
    g_guest.OnLobbyConnectedExit(self);
}

void* __fastcall LobbyConnectedUpdateDetour(void* self, void* transition) {
    if (g_guest.IsGuestLobby(self)) {
        EnsureGuestReplicaConnection();
        return GameFunction<UpdateMethod>(rva::kLocalHostUpdate)(self, transition);
    }
    return g_hooks.connectedUpdate.Original<UpdateMethod>()(self, transition);
}

void __fastcall MultiplayerIdleEnterDetour(void* self) {
    g_hooks.multiplayerIdleEnter.Original<VoidMethod>()(self);
    g_guest.OnMultiplayerIdleEnter();
}

bool InstallHooks() {
    struct Spec { PointerHook& hook; uintptr_t vtable; size_t slot; uintptr_t expected; void* detour; };
    const Spec specs[] = {
        {g_hooks.lobbyIdleUpdate, rva::kLobbyIdleVtable, 10, rva::kLobbyIdleUpdate, &LobbyIdleUpdateDetour},
        {g_hooks.localHostEnter, rva::kLocalHostVtable, 5, rva::kLocalHostEnter, &LocalHostEnterDetour},
        {g_hooks.localHostExit, rva::kLocalHostVtable, 6, rva::kLocalHostExit, &LocalHostExitDetour},
        {g_hooks.connectedEnter, rva::kConnectedVtable, 5, rva::kConnectedEnter, &LobbyConnectedEnterDetour},
        {g_hooks.connectedExit, rva::kConnectedVtable, 6, rva::kConnectedExit, &LobbyConnectedExitDetour},
        {g_hooks.connectedUpdate, rva::kConnectedVtable, 10, rva::kConnectedUpdate, &LobbyConnectedUpdateDetour},
        {g_hooks.multiplayerIdleEnter, rva::kMultiplayerIdleVtable, 5, rva::kMultiplayerIdleEnter, &MultiplayerIdleEnterDetour},
        {g_hooks.rakPeerStartup, rva::kRakPeerVtable, 1, rva::kRakPeerStartup, &RakPeerStartupDetour},
        {g_hooks.networkConnectingEnter, rva::kNetworkConnectingVtable, 5, rva::kNetworkConnectingEnter, &NetworkConnectingEnterDetour},
        {g_hooks.networkConnectedEnter, rva::kNetworkConnectedVtable, 5, rva::kNetworkConnectedEnter, &NetworkConnectedEnterDetour},
        {g_hooks.replicaNewConnection, rva::kReplicaManagerVtable, 8, rva::kReplicaOnNewConnection, &ReplicaNewConnectionDetour},
    };
    for (const Spec& spec : specs) {
        if (!HookVtableSlot(spec.hook, spec.vtable, spec.slot, spec.expected, spec.detour)) {
            for (PointerHook* hook : g_hooks.All()) hook->Restore();
            return false;
        }
    }
    g_hooksInstalled.store(true, std::memory_order_release);
    return true;
}

class EpicPlugin final : public IPluginInterface {
public:
    ~EpicPlugin() override {
        g_room.Stop();
        g_guest.Stop();
        if (g_observedHostState != nullptr) SetHostLobbyObserver(g_observedHostState, false);
        StopHostProxy();
        for (PointerHook* hook : g_hooks.All()) hook->Restore();
        g_hooksInstalled.store(false, std::memory_order_release);
    }

    void Init() override {
        m_backendReady = m_backend.Apply(Storefront::Epic);
        g_gameVersion = ReadGameVersion();
        // The room client delivers guest traffic here; native RakNet keeps UDP 30000.
        StartHostProxy(kHostProxyPort);
        ReportOpenNatToEos();
    }

    void OnEngineInitialized() override {
        m_backendReady = m_backend.Apply(Storefront::Epic);
        if (!g_hooksInstalled.load(std::memory_order_acquire)) InstallHooks();
    }

    void OnDrawUI(bool) override { g_roomClose.Tick(g_room); }

    void OnDrawMenu() override {
        const bool hooks = g_hooksInstalled.load(std::memory_order_acquire);
        ui::RoomPanelState panel{"Epic — host or join a cross-store lobby"};
        panel.notice = hooks ? "" : "Unsupported game version; crossplay is disabled.";
        panel.backendStatus = m_backend.Status();
        panel.room = g_room.Get();
        const bool busy = rooms::IsBusy(panel.room.state);
        panel.canHost = m_backendReady && IsHostLobbyReady() && !busy;
        if (hooks && !busy && !panel.canHost) {
            panel.notice = HostLobbyBlocker();
            const char* transport = g_transportIssue.load();
            if (panel.notice.ends_with("transport") && transport[0]) panel.notice += std::string(" (") + transport + ")";
        }
        panel.canJoin = m_backendReady && hooks && !busy;
        panel.showClose = busy;

        std::string code;
        switch (ui::DrawRoomPanel(panel, code)) {
            case ui::RoomPanelAction::Host:
                g_guest.CancelForHosting();
                g_room.Start(true, {}, kHostProxyPort, {}, "epic");
                break;
            case ui::RoomPanelAction::Join:
                g_guest.Join(code);
                break;
            case ui::RoomPanelAction::Close:
                g_room.Cancel();
                g_guest.Abandon();
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

IPluginInterface* CreatePlugin() { return new EpicPlugin(); }

} // namespace ghost::epic
