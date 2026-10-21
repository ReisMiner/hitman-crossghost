#pragma once

class IPluginInterface;

namespace ghost {
namespace epic { IPluginInterface* CreatePlugin(); }
namespace steam { IPluginInterface* CreatePlugin(); }
} // namespace ghost
