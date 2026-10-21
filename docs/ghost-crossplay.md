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

With the GhostCrossplay DLL and Ghost Mode assets installed, the Epic player
selects a map/lobby and presses **Host**. They share the eight-character room code.
The Steam player enters that code and presses **Join**. The DLL configures the
game backend automatically. Epic hosting/Steam joining is the implemented store
direction; reverse-store hosting remains separate work.

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
