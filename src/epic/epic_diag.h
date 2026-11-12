#pragma once

namespace ghost::epic {

// Diagnostics only: logs the native session state (RakNet identity, network
// role, ZPlayerRegistry slots, ready table, backend match) when it changes.
// Read-only; call on the game thread, at most once per second is logged.
void LogSessionState();

} // namespace ghost::epic
