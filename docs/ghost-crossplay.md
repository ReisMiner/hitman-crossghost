# Ghost Crossplay: one LocalGhost container

This fork runs the game backend, room-code API and authenticated UDP relay in
one Node process. Everything needed to deploy is inside this repository.

## Deploy

From the fork's root:

```sh
docker compose up -d --build
```

The defaults are ready for **https://hitmanghost.ramon.cc**:

- **TCP 47047:** the single HTTP upstream for your existing HTTPS proxy. Forward
  the whole domain here, preserving request paths.
- **UDP 30002:** game relay. Publish/allow this port directly on the VPS.

The domain's IPv4 address must reach this VPS for UDP as well as HTTP. Players
connect through room codes; they never enter ports or forward their own routers.

Check the deployment:

```sh
docker compose ps
docker compose logs --tail=100
curl --fail https://hitmanghost.ramon.cc/healthz
curl --fail https://hitmanghost.ramon.cc/config/pc-prod/8_2_0
```

Health returns `ok`. All game config/resource URLs and room responses use
`https://hitmanghost.ramon.cc`, independent of incoming Host/protocol headers.

### Optional settings

Defaults require no `.env` file. Compose accepts these overrides if needed:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PUBLIC_ORIGIN` | `https://hitmanghost.ramon.cc` | Shared public origin, without a path |
| `HTTP_PORT` | `47047` | Published HTTP upstream port |
| `RELAY_PORT` | `30002` | Published and advertised UDP relay port |
| `TRUST_PROXY` | `single` | One trusted proxy hop for per-client rate limits; `loopback` and `none` also work |

Changing the public origin also requires rebuilding the player DLL with the same
origin. Keep the container on Node 22 for the existing LocalGhost dependency set.

### Profiles and updates

Profiles persist in the named volume `localghost_userdata`. Fresh volumes start
with the reference templates. Local player profiles and identity bindings are
excluded from the image; import existing userdata separately when migrating.

Run `docker compose up -d --build` to deploy updated sources. Active room codes
are in memory and end when the process restarts. `docker compose down` retains
profiles; adding `-v` deletes the volume.

For a consistent backup:

```sh
docker compose stop localghost
docker compose run --rm --no-deps -T localghost tar -C /app/userdata -czf - . > userdata-backup.tar.gz
docker compose start localghost
```

## Player flow

With the GhostCrossplay DLL and Ghost Mode assets installed, either storefront
selects a map/lobby and presses **Host**. They share the five-digit room code.
The other player enters that code and presses **Join**. The DLL configures the
game backend automatically. Both Epic-host/Steam-guest and Steam-host/Epic-guest
are supported.

### Hawke's Bay

The H3 menu includes Miami, Santa Fortuna, Hawke's Bay, Haven Island and Paris,
including the Random Map pool. Hawke's Bay uses the Ghost Mode mod's daytime **Anubis** mission, not
the Nightcall story contract. Both PCs need Ghost Mode assets **2.1.0 or newer**
(2.3.0 recommended), which include the Hawke's Bay entities.

After updating the server sources, run `docker compose up -d --build` and restart
both games to refresh the map menu. Install the updated universal DLL on both
PCs; the Epic guest needs its new Hawke's Bay preset-to-contract fallback.
There are no additional containers, ports or player settings.

Definitions:

- `Server/contractdata/8fcf8f5d-0fe9-47d7-bff3-d747d0cd1e89.json`: the Anubis
  scene, Ghost Mode bricks, objective and scoring. It uses the same objective
  state machine as the existing Ghost Mode contracts. The mission ID and asset
  list were checked against Peacock's `contractdata/GHOST/ANUBIS.json` and the
  HITMAPS Ghost Mode mod's `manifest.json`.
- `Server/menudata/h3/menudata/multiplayerpresets.json`: the dedicated Hawke's
  Bay invite preset (ID equal to its contract ID) and the all-map pools.
- The earlier experimental `GhostLobbyService` map list is not used by the DLL;
  the native game gets its maps through `/multiplayerpresets`.

Automated tests cover menu data, required resources and match registration in
both store directions. Two-PC gameplay still needs a manual test.

### Haven Island — one merged SMF mod

Haven requires the workspace's merged `HITMAPS.GhostModeEnabler` **2.4.0**
edition on both PCs. It contains Adam's `Ghost-Mode-Haven` gameplay assets;
do not install/enable `Adam.GhostHaven` as a second mod. Replace the existing
Ghost Mode folder in SMF's Mods directory and deploy on both PCs. Players still
need their own installed Haven Island map content.

Deploy the server with `docker compose up -d --build`, install the updated
universal DLL on both PCs, and restart both games. The Haven Island invite tile
follows Hawke's Bay; Haven is also included in the Random Map pool.

- Contract and dedicated preset: `5e895adf-9879-44d9-b43f-13bb24e897bc`.
- Scene: `assembly:/_pro/scenes/missions/opulent/mission_stingray/scene_stingray_basic.entity`.
- Location: `LOCATION_OPULENT_STINGRAY`; entitlement: `LOCATION_OPULENT`.
- Bricks: Haven's `versus_targets_opulent_stingray`,
  `versus_items_opulent_stingray`, `versus_entrance_opulent_stingray`, plus
  shared `ui_versus` and `vs_race_multiplayer`.
- The merged targets asset includes the
  `fDynamicScaleCloseupDistance: 0.0` field required by the current game; the
  original standalone Haven asset fails SMF deployment without it.
- Objective and five-point scoring follow the existing versus contracts; the
  story contract is not reused as a playable versus contract.

The original Haven assets were advertised for Steam only. Offline tests cover
the merged package and both server-side host directions, but manual SMF
deployment and two-PC gameplay are still required to validate Steam/Epic Haven.

### Paris

Paris is included in the same **2.5.0** mod package, so there is still only one
SMF mod to install. Deploy the updated package and the updated server, then
install the updated universal DLL on both PCs and restart both games. Paris
appears in **Invite Player** after Haven and is part of Random Map.

- Contract and dedicated preset: `59d1caab-2056-4f4b-a921-9958ef0a88eb`.
- Scene: `assembly:/_pro/scenes/missions/paris/_scene_paris.entity`.
- Location: `LOCATION_PARIS`; entitlement: `LOCATION_PARIS`.
- Bricks: `versus_targets_paris`, `versus_items_paris`,
  `versus_entrance_paris`, plus shared `ui_versus` and `vs_race_multiplayer`.
- The asset mod adds 24 player starts in 12 public-ground-floor pairs, 24 mixed
  item crates (eight each of Ghost equipment, tools/poisons and weapons) and
  42 new target NPCs with three-stop local patrol loops. Their
  coordinates are checked against native access volumes, navigation and exported
  map geometry; runtime collision and two-PC gameplay still need manual testing.
  The story targets and native scene are left intact.
- The target picker is the same H3 versus composite used by Hawke's Bay. Its
  target list, roles and drama situations are authored in the Paris targets
  brick; the existing `vs_race_multiplayer` rules provide selection/scoring.

Automated tests validate the three brick documents, every local entity
reference, resource registrations, server menu/resource responses and both
host directions. Actual SMF deployment, spawn points, target reachability and
cross-store gameplay still need a manual two-PC test.

## Implementation and tests

- `Server/serb.js` mounts `/healthz`, `/v1/config` and `/v1/rooms/*` before the
  game-token middleware, on the same HTTP listener as all LocalGhost endpoints.
- `Server/components/ghostRooms/` contains the binary room protocol and UDP relay.
  It preserves native RakNet/app payloads and uses per-member HMAC-SHA256 tokens,
  replay rejection, NAT rebinding, leases, two-player capacity and bounded traffic.
- Existing game-facing Ghost Mode endpoints and the earlier experimental
  `GhostLobbyService` remain in their existing components. The DLL uses `/v1/rooms/*`.
- The HTTP listener starts only after the UDP socket binds successfully. SIGTERM
  closes the shared listener and relay together.

Run the backend/room integration tests with Node 22:

```sh
cd Server
npm ci --omit=dev --ignore-scripts
npm test
```

For a direct local development run, set `PUBLIC_ORIGIN=http://127.0.0.1:47047`,
`PORT=47047`, then run `npm start` from `Server/`. The native development DLL must
use that same origin.

Current validation: Compose configuration and all four backend/room integration
tests passed under Node 22. Actual container startup remains untested locally
because the Docker daemon is stopped; public two-network gameplay is pending.
