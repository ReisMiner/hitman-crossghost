#pragma once

#include "net/room_client.h"

#include <string>

namespace ghost::ui {

struct RoomPanelState {
    const char* title;            // e.g. "Epic — host or join a cross-store lobby"
    std::string notice;           // optional line under the title (plugin status)
    std::string backendStatus;
    rooms::Snapshot room;
    bool canHost = false;
    bool canJoin = false;
    bool showClose = false;
};

enum class RoomPanelAction { None, Host, Join, Close };

// Draws the "CrossGhost" SDK window. For Join, `joinCode` holds the
// entered five-digit code.
RoomPanelAction DrawRoomPanel(const RoomPanelState& state, std::string& joinCode);

} // namespace ghost::ui
