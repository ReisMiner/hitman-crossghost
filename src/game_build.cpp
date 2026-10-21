#include "game_build.h"

#include "common/sha256.h"

#include <Windows.h>

#include <string>

namespace ghost {

const std::string& GameExecutableSha256() {
    static const std::string hash = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::string result;
        if (length == 0 || length >= MAX_PATH || !Sha256File(path, result)) result.clear();
        return result;
    }();
    return hash;
}

} // namespace ghost
