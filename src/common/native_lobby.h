#pragma once

// Layout shared by the Epic and Steam builds of HITMAN 3.290.1's native lobby
// state machine (ZLobbyIdle, ZLobbyCreateLocalHost, ZLobbyConnected, ...).

#include "common/memory.h"

#include <cstdint>

namespace ghost::lobby {

// Values of the lobby request field read by ZLobbyIdle's update.
enum Request : int {
    kLeave = 1,
    kCreate = 2,      // platform lobby (EOS/Steam); cannot cross storefronts
    kLocalHost = 3,   // plain RakNet lobby; what cross-store hosting uses
    kJoin = 4,
};

// Every lobby state keeps its shared lobby context at +0x20.
inline void* Context(void* state) {
    return state != nullptr ? ReadPointer(FieldPointer<void>(state, 0x20)) : nullptr;
}

// The pending request lives at context +0x50; nullptr when unreadable.
inline int* RequestField(void* state) {
    void* context = Context(state);
    if (context == nullptr) return nullptr;
    int* field = FieldPointer<int>(context, 0x50);
    return IsReadable(field, sizeof(int)) ? field : nullptr;
}

inline int ReadRequest(void* state) {
    const int* field = RequestField(state);
    return field != nullptr ? *field : -1;
}

// Lobby-data writes are broadcast through a delegate list at context +0x150.
constexpr size_t kLobbyDataDelegatesOffset = 0x150;

// Native delegate record passed to the engine's delegate add/remove functions:
// {free function, this-adjustment, member function, bound object}.
struct Delegate {
    uintptr_t function;
    uintptr_t adjustment;
    uintptr_t method;
    uintptr_t self;
};

using DelegateListFn = void*(__fastcall*)(void* list, Delegate* delegate);

} // namespace ghost::lobby
