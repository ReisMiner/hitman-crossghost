#include "common/diag_log.h"

#include <Windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace ghost::diag {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

std::wstring Directory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::wstring(buffer, length) + L"\\CrossGhost";
}

} // namespace

void Open() {
    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) return;
    const std::wstring directory = Directory();
    if (directory.empty()) return;
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring path = directory + L"\\CrossGhost.log";
    MoveFileExW(path.c_str(), (directory + L"\\CrossGhost.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Write(std::string_view line) {
    SYSTEMTIME time;
    GetLocalTime(&time);
    std::lock_guard lock(g_mutex);
    if (g_file == nullptr) return;
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [%5lu] %.*s\n", time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, GetCurrentThreadId(), static_cast<int>(line.size()), line.data());
    std::fflush(g_file);
}

std::string Brief(std::string_view value, size_t keep) {
    if (value.size() <= keep) return std::string(value);
    return std::string(value.substr(0, keep)) + "...(" + std::to_string(value.size()) + " chars)";
}

} // namespace ghost::diag
