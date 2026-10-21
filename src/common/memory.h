#pragma once

// Helpers for reading and patching the running HITMAN process.

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ghost {

// Base address of HITMAN3.exe. All game offsets in this project are RVAs.
uintptr_t GameBase();

template <typename T = void>
T* GameAddress(uintptr_t rva) {
    return reinterpret_cast<T*>(GameBase() + rva);
}

// A game function at `rva`, e.g. GameFunction<void(__fastcall*)(void*)>(0x1234).
template <typename Fn>
Fn GameFunction(uintptr_t rva) {
    return reinterpret_cast<Fn>(GameBase() + rva);
}

// True when [address, address + size) is committed, accessible memory.
// Uses VirtualQuery, so keep it off per-packet hot paths.
bool IsReadable(const void* address, size_t size);

template <typename T>
T* FieldPointer(void* object, size_t offset) {
    return reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

// Reads a pointer-sized field, or nullptr when the memory is not readable.
void* ReadPointer(const void* address);

// A single replaced function pointer: a vtable slot or an import table entry.
// Restore() only writes the original back if our detour is still installed.
class PointerHook {
public:
    // Replaces *entry with detour. When expected is set, the current value must
    // match it, which guards against running on an unexpected game build.
    bool Install(void** entry, void* detour, void* expected = nullptr);
    void Restore();
    bool Installed() const { return m_entry != nullptr; }

    template <typename Fn>
    Fn Original() const { return reinterpret_cast<Fn>(m_original); }

private:
    void** m_entry = nullptr;
    void* m_original = nullptr;
    void* m_detour = nullptr;
};

// Hooks slot `index` of the game vtable at `vtableRva`, which must currently
// point at the game function `expectedRva`.
bool HookVtableSlot(PointerHook& hook, uintptr_t vtableRva, size_t index, uintptr_t expectedRva, void* detour);

// Overwrites the start of a function with `mov rax, replacement; jmp rax`.
// Permanent: used for small exported functions we never need to call again.
bool RedirectFunction(void* target, void* replacement);

// Writes raw bytes into executable memory.
bool WriteCode(void* address, const void* bytes, size_t size);

// The engine's ZString layout: length in the low 30 bits, then a char pointer.
struct GameString {
    uint32_t length;
    uint32_t padding;
    const char* chars;
};

constexpr uint32_t kStaticStringFlag = 0x80000000u;

// Copies a game string into `value`. Fails on unreadable or oversized strings.
bool ReadGameString(const void* pointer, std::string& value);

} // namespace ghost
