#pragma once

#include "backend/backend_patch.h"

#include <atomic>
#include <string>

class IPluginInterface;

namespace ghost {

// The backend patch plus the server chosen in the settings panel, saved in the
// plugin's SDK settings as [General] Server = hitmanghost | ioi. A changed
// selection takes effect when the game is restarted.
class BackendController {
public:
    BackendController(IPluginInterface& plugin, Storefront store) : m_plugin(plugin), m_store(store) {}

    // Game thread (Init, OnEngineInitialized). Patches the server saved at
    // startup; selections made later wait for a restart.
    void Apply();
    // Settings panel (Apply server): saves the selection for the next start.
    void Select(BackendServer server);
    // The last Select could not be read back from the settings file.
    bool SaveFailed() const { return m_saveFailed.load(std::memory_order_acquire); }

    BackendServer Selected() const { return m_selected.load(std::memory_order_acquire); }
    // The server patched in at startup, which stays until the game restarts.
    BackendServer Running() const { return m_running.load(std::memory_order_acquire); }
    bool RestartRequired() const { return Selected() != Running(); }
    // True when the HitmanGhost backend is patched in; crossplay needs it.
    bool CrossplayReady() const { return m_ready.load(std::memory_order_acquire); }
    std::string Status() const { return m_patch.Status(); }

private:
    void Load();

    IPluginInterface& m_plugin;
    const Storefront m_store;
    BackendPatch m_patch;
    std::atomic<BackendServer> m_selected{BackendServer::HitmanGhost};
    std::atomic<BackendServer> m_running{BackendServer::HitmanGhost};
    std::atomic<bool> m_loaded{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_saveFailed{false};
};

} // namespace ghost
