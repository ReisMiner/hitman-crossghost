# Hitman Ghost Mode Crossplay

A [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) plugin that lets an Epic
and a Steam player play HITMAN 3 Ghost Mode together.

## Installation and Usage for Gamers

To play ghost mode with crossplay enabled, you'll need the following requirements installed. Consult the respective installation guide for each of those:

- [Simple Mod Framework](https://www.nexusmods.com/hitman3/mods/200)
- [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK)
- [Ghost Mode Assets Mod](https://git.ramon.cc/ramon/crossghost-assets/releases/latest) deployed with SMF
- GhostCrossplay.dll downloaded from the [release page](https://git.ramon.cc/ramon/crossghost/releases/latest) and copied into `<Hitman Install Folder>/Retail/mods/`
- LocalGhost Patcher is **NOT NEEDED!**

### Activating and Disabling the Mod

Activating the mod is easily done by opening the ZHMModSDK Interface by pressing <kbd>insert</kbd>. Then open the mod list and enable CrossGhost.

When toggling the mod on or off, you will **need to restart the game** for smooth operation. It can get buggy otherwise.

### Hosting a lobby

To host a lobby, follow these steps:

1. Open Ghost Mode from main menu
2. Go to Invite Player to the right and select a map
3. When in the lobby, press <kbd>insert</kbd> to open ZHMModSDK and click the Host Button in the CrossGhost mod.
4. Send the 5 Digit code to your friend.
5. Both players press ready in Hitman and the match will begin.

### Joining a lobby

To join a lobby, follow these steps:

1. Open Ghost Mode from main menu
2. Press <kbd>insert</kbd> to open ZHMModSDK and enter the 5 Digit Code in the Text field from the CrossGhost mod.
3. Click join and wait a few seconds until you're in the lobby and can see the other player.
4. Ready up, when you're ready. The match will begin when both players are ready.

## How it works

```
host game (RakNet :30000) ⇄ host proxy (127.0.0.1:30001) ⇄ room client
    ⇄ LocalGhost relay (UDP 30002) ⇄
room client (127.0.0.1:<random>) ⇄ guest game
```

1. **Backend.** At startup the DLL patches the game's backend configuration so
   both storefronts use the LocalGhost server (`src/backend/`).
2. **Hosting.** The game's "create lobby" request (EOS or Steam lobby) is
   rewritten to "local host", a plain RakNet lobby. The plugin records the
   lobby data the guest will need (map, game mode, host key, version) and
   serves it through the host proxy (`src/net/host_proxy.*`).
3. **Room.** Pressing **Host** creates a room on the server and shows its code.
   The room client (`src/net/room_client.*`) keeps an outbound UDP tunnel to
   the server's relay, so no inbound ports are needed.
4. **Joining.** The guest joins the room, fetches the host's lobby metadata
   through the tunnel (`src/net/metadata_fetcher.*`) and then drives the game's
   own join flow:
   - **Epic** (`src/epic/epic_guest.*`): queues a native "accept invite" and
     serves the metadata through a lobby-data provider.
   - **Steam** (`src/steam/`): fakes a Steam lobby and Steam P2P session
     (`steam_lobby_shim.*`) and dispatches a native invite to it.
5. **Leaving.** When either player leaves, the native RakNet disconnect is
   relayed before the room closes, so the other side leaves its lobby at once.

## Source layout

```
src/plugin_entry.cpp      SDK exports; picks the Epic or Steam implementation
src/game_build.*          Supported executables (SHA-256) and storefront detection
src/backend/              Backend-origin patch (replaces the LocalGhost patcher)
src/common/               Memory access, vtable/IAT hooks, game strings, lobby layout
src/net/                  Room client, relay protocol, host proxy, metadata codec
src/epic/                 Epic hooks; epic_offsets.h lists every game address used
src/steam/                Steam hooks and lobby shim; steam_offsets.h lists addresses
src/ui/room_panel.*       The "Ghost Crossplay" SDK window shared by both builds
tests/                    Codec/proxy, room tunnel and installer tests
scripts/                  Dependency download and game installation
```

Game addresses are RVAs for the exact executables above. If the game updates,
every value in `epic_offsets.h`, `steam_offsets.h` and the backend layouts in
`backend_patch.cpp` must be re-derived; hook installation refuses to run if a
vtable slot does not contain the expected function.

## Build

Native Windows requirements: Visual Studio 2022 (x64 C++), CMake 3.20+, Git,
PowerShell 7, and Node.js 20+ for the room tunnel test.

```powershell
./scripts/fetch-deps.ps1          # once: ZHMModSDK 4.1.1 and DirectX-Headers
./build.ps1 -NoPublish            # build/Release/GhostCrossplay.dll
./build.ps1                       # also copies the DLL to dist/
ctest --test-dir build -C Release --output-on-failure
```

`-PublicOrigin` selects the LocalGhost server compiled into the DLL (default
`https://hitmanghost.ramon.cc`). Close HITMAN before running the tests; they
bind UDP 30000. The `room-tunnel` test needs a LocalGhost checkout next to this
folder (`../localghost`, or set `GHOST_LOCALGHOST_DIR`).

### Building on Linux

The same MSVC-ABI DLL can be cross-compiled on Linux with clang-cl, lld-link
and the MSVC CRT/Windows SDK from [xwin](https://github.com/Jake-Shadle/xwin):

```bash
# once: unpack the CRT and Windows SDK (or set XWIN_ROOT)
xwin --accept-license --cache-dir /root/.xwin-cache splat --output /xwin

bash scripts/fetch-deps.sh
bash scripts/build-linux.sh        # dist/GhostCrossplay.dll
ctest --test-dir build-linux --output-on-failure
```

Requirements: clang/lld 19+, CMake 3.20+, Ninja, Git, unzip, and Wine for the
tests. `CONFIG`, `GHOST_PUBLIC_ORIGIN`, `BUILD_DIR` and `XWIN_ROOT` can be set as
environment variables. The `install-script` and `room-tunnel` tests need a
native Windows host, so only `relay-metadata` runs here.

## Releases

Merging to `main` runs `.forgejo/workflows/release.yml` on a `docker` runner in
`messense/cargo-xwin:0.23.1` (clang 19, lld, CMake, Ninja and Wine). It installs
the xwin MSVC CRT/Windows SDK, cross-builds `dist/GhostCrossplay.dll`, runs the
tests under Wine and publishes a `v0.<run_number>` release with the DLL
attached. The release notes are generated from the commits since the previous
release, including the merge request numbers referenced in their titles.

## Install

Install the same DLL on both PCs. ZHMModSDK 4.1.1 and the Ghost Mode assets must
already be installed. Close HITMAN, then (elevated for Program Files):

```powershell
./scripts/install.ps1 -GameRoot 'C:\Program Files\Epic Games\HITMAN3'
./scripts/install.ps1 -GameRoot 'C:\Program Files (x86)\Steam\steamapps\common\HITMAN 3'
```

The script copies `dist/GhostCrossplay.dll` into `Retail/mods`, backs up older
plugin DLLs and enables `[GhostCrossplay]` in `Retail/mods.ini`.

## Use

- **Host:** open Ghost Mode and choose a map. In the **Ghost Crossplay** SDK
  window press **Host** and share the room code.
- **Join:** open Ghost Mode, enter the code in the **Ghost Crossplay** window
  and press **Join**.

Do not run the Epic and Steam games on the same PC at the same time; both use
UDP 30000 and 30001.
