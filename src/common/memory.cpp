#include "common/memory.h"

#include <cstring>

namespace ghost {

uintptr_t GameBase() {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base;
}

bool IsReadable(const void* address, size_t size) {
    if (address == nullptr || size == 0) return false;
    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + size;
    if (end < cursor) return false;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

void* ReadPointer(const void* address) {
    return IsReadable(address, sizeof(void*)) ? *static_cast<void* const*>(address) : nullptr;
}

bool PointerHook::Install(void** entry, void* detour, void* expected) {
    if (m_entry != nullptr) return m_entry == entry && *entry == detour;
    if (!IsReadable(entry, sizeof(void*)) || (expected != nullptr && *entry != expected)) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    m_entry = entry;
    m_original = *entry;
    m_detour = detour;
    InterlockedExchangePointer(entry, detour);
    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(void*), oldProtect, &ignored);
    return true;
}

void PointerHook::Restore() {
    if (m_entry == nullptr) return;
    DWORD oldProtect = 0;
    if (*m_entry == m_detour && VirtualProtect(m_entry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        InterlockedExchangePointer(m_entry, m_original);
        DWORD ignored = 0;
        VirtualProtect(m_entry, sizeof(void*), oldProtect, &ignored);
    }
    *this = {};
}

bool HookVtableSlot(PointerHook& hook, uintptr_t vtableRva, size_t index, uintptr_t expectedRva, void* detour) {
    auto** entry = GameAddress<void*>(vtableRva + index * sizeof(void*));
    return hook.Install(entry, detour, GameAddress(expectedRva));
}

bool WriteCode(void* address, const void* bytes, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD ignored = 0;
    return VirtualProtect(address, size, oldProtect, &ignored) != 0;
}

bool RedirectFunction(void* target, void* replacement) {
    if (target == nullptr || replacement == nullptr) return false;
    uint8_t code[12] = {0x48, 0xb8};                // mov rax, imm64
    const auto address = reinterpret_cast<uint64_t>(replacement);
    std::memcpy(code + 2, &address, sizeof(address));
    code[10] = 0xff;                                // jmp rax
    code[11] = 0xe0;
    return WriteCode(target, code, sizeof(code));
}

bool ReadGameString(const void* pointer, std::string& value) {
    if (!IsReadable(pointer, sizeof(GameString))) return false;
    const auto* raw = static_cast<const GameString*>(pointer);
    const uint32_t size = raw->length & 0x3fffffffu;
    if (size > (1u << 20)) return false;
    value.clear();
    if (size == 0) return true;
    if (!IsReadable(raw->chars, size)) return false;
    value.assign(raw->chars, size);
    return true;
}

} // namespace ghost
