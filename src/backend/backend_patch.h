#pragma once

#include "game_build.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ghost {

// Points HITMAN's online backend at the LocalGhost server compiled into this
// DLL (GHOST_BACKEND_URL) by patching the game's backend configuration in
// memory. Replaces the external LocalGhost patcher.
//
// Every patch site is signature-checked before anything is written, and all
// patches are reverted when this object is destroyed (SDK unload).
class BackendPatch {
public:
    ~BackendPatch();
    BackendPatch() = default;
    BackendPatch(const BackendPatch&) = delete;
    BackendPatch& operator=(const BackendPatch&) = delete;

    // Idempotent. Call on the game thread before online services start.
    bool Apply(Storefront store);
    std::string Status() const;

private:
    struct Patch {
        void* address;
        std::vector<uint8_t> original;
        std::vector<uint8_t> installed;
    };

    void Restore();

    std::vector<Patch> m_patches;
    mutable std::mutex m_mutex;
    std::string m_status = "Backend not configured";
};

} // namespace ghost
