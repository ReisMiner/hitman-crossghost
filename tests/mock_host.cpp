// Test-only room host: publishes a valid-looking lobby and serves metadata over
// the loopback host proxy. It does not implement native RakNet gameplay.
#include <Windows.h>
#include "net/host_proxy.h"
#include "net/room_client.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string origin = argc > 1 ? argv[1] : "";
    if (!ghost::StartHostProxy(30002)) { std::puts("host proxy failed"); return 1; }
    ghost::BeginHostLobby();
    ghost::PublishHostLobbyField("PresetId", "miami");
    ghost::PublishHostLobbyField("ContractId", "c854d9c7-8991-42db-a2e4-3cf93e645e81");
    ghost::PublishHostLobbyField("matchId", "8fd8cb9f-1f60-49b0-8fb7-85621285fd75");
    ghost::PublishHostLobbyField("gameMode", "versus");
    ghost::PublishHostLobbyField("HostGameVersion", "3.290.1.0");
    ghost::PublishHostLobbyField("HostPublicKey",
        "tI3lexI8AneS0kKQiplRqm0j3WDYxiHPOLN8qZ11CHDAJ/tK+1AAhAxLXXCedaf6GQlhfeVuE3tbdqB8XOTs2A==");
    ghost::SetHostLobbyTransportReady();
    ghost::rooms::Client host;
    if (!host.Start(true, {}, 30002, origin, "epic")) { std::puts("room start failed"); return 1; }
    for (int i = 0; i < 300; ++i) {
        const auto room = host.Get();
        if (room.state == ghost::rooms::State::Ready) {
            std::printf("MOCK_HOST_READY %s\n", room.code.c_str());
            std::fflush(stdout);
            Sleep(180000);
            break;
        }
        if (room.state == ghost::rooms::State::Failed) { std::printf("MOCK_HOST_FAILED %s\n", room.status.c_str()); break; }
        Sleep(100);
    }
    host.Stop();
    ghost::EndHostLobby();
    ghost::StopHostProxy();
    return 0;
}
