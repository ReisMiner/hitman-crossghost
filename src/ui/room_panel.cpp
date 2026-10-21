#include "ui/room_panel.h"

#include "IModSDK.h"

namespace ghost::ui {

RoomPanelAction DrawRoomPanel(const RoomPanelState& state, std::string& joinCode) {
    ImGui::SetNextWindowPos(ImVec2(120, 120), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Ghost Crossplay", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return RoomPanelAction::None;
    }

    RoomPanelAction action = RoomPanelAction::None;
    ImGui::TextUnformatted(state.title);
    ImGui::TextWrapped("%s", state.backendStatus.c_str());
    if (!state.notice.empty()) ImGui::TextWrapped("%s", state.notice.c_str());
    ImGui::TextWrapped("%s", state.room.status.c_str());
    if (!state.room.code.empty()) {
        ImGui::Text("Room code: %s", state.room.code.c_str());
        if (ImGui::Button("Copy room code")) ImGui::SetClipboardText(state.room.code.c_str());
    }

    ImGui::BeginDisabled(!state.canHost);
    if (ImGui::Button("Host")) action = RoomPanelAction::Host;
    ImGui::EndDisabled();

    static char codeInput[8] = {};
    ImGui::BeginDisabled(!state.canJoin);
    ImGui::InputText("Room code", codeInput, 6, ImGuiInputTextFlags_CharsDecimal);
    if (ImGui::Button("Join")) {
        joinCode = codeInput;
        action = RoomPanelAction::Join;
    }
    ImGui::EndDisabled();

    if (state.showClose) {
        ImGui::SameLine();
        if (ImGui::Button("Close room")) action = RoomPanelAction::Close;
    }
    ImGui::TextWrapped("Host: choose a map, press Host, share the code. "
        "Join: open Ghost Mode, enter the code, press Join.");
    ImGui::End();
    return action;
}

} // namespace ghost::ui
