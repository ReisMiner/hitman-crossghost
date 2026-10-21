#pragma once

#include "net/room_client.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace ghost {

// Closes a room a short time after it is scheduled. Used when leaving a lobby:
// the game queues its RakNet disconnect on the network thread, and the relay
// must stay up long enough for it to reach the other player. That lets the
// other side close its lobby right away instead of waiting for a timeout.
class DeferredRoomClose {
public:
    static constexpr uint64_t kDelayMs = 2000;

    void Schedule() { m_due.store(GetTickCount64() + kDelayMs, std::memory_order_release); }
    void Clear() { m_due.store(0, std::memory_order_release); }
    // Clears a pending close; returns true if one was pending.
    bool TakePending() { return m_due.exchange(0) != 0; }

    // Call regularly; cancels the room once the delay has passed.
    void Tick(rooms::Client& room) {
        uint64_t due = m_due.load(std::memory_order_acquire);
        if (due != 0 && GetTickCount64() >= due && m_due.compare_exchange_strong(due, 0)) room.Cancel();
    }

private:
    std::atomic<uint64_t> m_due{0};
};

} // namespace ghost
