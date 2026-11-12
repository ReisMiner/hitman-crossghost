#pragma once

// Diagnostic log for multiplayer troubleshooting. Each game session writes
// %LOCALAPPDATA%\CrossGhost\CrossGhost.log (the previous one is kept as
// CrossGhost.old.log). Safe to call from any thread.

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ghost::diag {

void Open();
void Write(std::string_view line);

template <class... Args>
void Log(std::format_string<Args...> format, Args&&... args) {
    Write(std::format(format, std::forward<Args>(args)...));
}

// Shortens long values (keys, addresses) for the log.
std::string Brief(std::string_view value, size_t keep = 48);

} // namespace ghost::diag
