#include "IModSDK.h"
#include "IPluginInterface.h"

#include "backend/backend_controller.h"

#include <string_view>

namespace ghost {
namespace {

// Lowercase on purpose: the SDK's ini writer lowercases names in
// mods/crossghost.ini while its in-memory lookup is case-sensitive, so
// mixed-case names never read back what was saved.
constexpr char kSection[] = "general";
constexpr char kKey[] = "server";
constexpr char kHitmanGhost[] = "hitmanghost";
constexpr char kIoi[] = "ioi";

} // namespace

void BackendController::Load() {
    if (m_loaded.exchange(true, std::memory_order_acq_rel)) return;
    const ZString saved = m_plugin.GetSetting(kSection, kKey, kHitmanGhost);
    const bool ioi = std::string_view(saved) == kIoi;
    const BackendServer server = ioi ? BackendServer::Ioi : BackendServer::HitmanGhost;
    m_selected.store(server, std::memory_order_release);
    m_running.store(server, std::memory_order_release);
}

// Patches the server saved at startup; later selections wait for a restart.
void BackendController::Apply() {
    Load();
    m_ready.store(m_patch.Apply(m_store, Running()), std::memory_order_release);
}

void BackendController::Select(BackendServer server) {
    Load();
    // ZString takes char arrays, so a ternary (which decays them) does not convert.
    if (server == BackendServer::Ioi) m_plugin.SetSetting(kSection, kKey, kIoi);
    else m_plugin.SetSetting(kSection, kKey, kHitmanGhost);
    // SetSetting writes the ini immediately; read it back to confirm.
    const ZString saved = m_plugin.GetSetting(kSection, kKey, "");
    const std::string_view expected = server == BackendServer::Ioi ? kIoi : kHitmanGhost;
    const bool ok = std::string_view(saved) == expected;
    m_saveFailed.store(!ok, std::memory_order_release);
    if (ok) m_selected.store(server, std::memory_order_release);
}

} // namespace ghost
