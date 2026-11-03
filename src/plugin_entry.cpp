// ZHMModSDK entry points. One DLL serves both storefronts: the running
// executable is identified by SHA-256 and the matching implementation is
// returned directly, so the SDK owns its lifecycle, UI and settings.

#include "IModSDK.h"
#include "IPluginInterface.h"

#include "game_build.h"
#include "plugin_entry.h"

#include <string>
#include <utility>

namespace ghost {
namespace {

class UnsupportedPlugin final : public IPluginInterface {
public:
    explicit UnsupportedPlugin(std::string reason) : m_reason(std::move(reason)) {}

    void OnDrawMenu() override {
        if (ImGui::Begin("CrossGhost", nullptr, ImGuiWindowFlags_NoCollapse))
            ImGui::TextWrapped("%s", m_reason.c_str());
        ImGui::End();
    }

private:
    std::string m_reason;
};

IPluginInterface* CreatePlugin() {
    if (GameExecutableSha256().empty())
        return new UnsupportedPlugin("Could not identify HITMAN executable; no backend loaded.");
    switch (DetectStorefront()) {
        case Storefront::Epic: return epic::CreatePlugin();
        case Storefront::Steam: return steam::CreatePlugin();
        default: return new UnsupportedPlugin("Unsupported HITMAN version; no backend loaded.");
    }
}

} // namespace
} // namespace ghost

extern "C" __declspec(dllexport) IPluginInterface* GetPluginInterface() {
    static IPluginInterface* instance = ghost::CreatePlugin();
    return instance;
}

extern "C" __declspec(dllexport) const char* CompiledSdkVersion() { return ZHMMODSDK_VER; }
extern "C" __declspec(dllexport) int CompiledSdkAbiVersion() { return ZHMMODSDK_ABI_VER; }
