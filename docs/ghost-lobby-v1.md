# Ghost lobby control plane v1 (experimental)

**Implemented: a store-independent, two-player HTTP lobby backend. Not implemented:
native game integration, automatic matchmaking, gameplay replication or crossplay.**
The stock game does not call these endpoints. Its Invite Player menu will still hit
the native Epic lobby barrier; this change is not a fix for the current timeout.

**Companion UI milestone:** `../../tools/GhostLobbyClient/README.md` describes the
separate Windows Host/Join-code window now built against this contract. It supports
map/readiness/Start intent and is tested using fake HTTP plus the unchanged store
in memory. It stays literal-loopback-only, takes an existing manually supplied
token, and has no game integration, protected LAN deployment or new login proof.

## Identity and deployment

- Use the UUID profile ID issued by LocalGhost's `/oauth/token`, not a Steam ID or
  Epic account ID. Platform is informational and is not used for room compatibility.
- Existing profile-to-store bindings remain unchanged. This does not automatically
  merge one person's Steam and Epic profiles or add account linking.
- One active membership per profile, two players per room. Host/join codes are the
  first milestone; there is no public lobby directory or automatic matchmaking.
- Rooms live in one Node process's memory. Restarting loses all rooms. Multiple
  server workers/replicas require shared state and atomic membership updates first.
- Node 22 is the tested runtime. No new dependencies are required.
- The currently running development server is not automatically restarted by this
  source change. Tests start separate servers on ephemeral loopback ports.

**Keep this development server loopback-only.** The new API checks HS256 signature,
issuer, audience and expiry of LocalGhost tokens, but LocalGhost still uses a known
hard-coded key and does not validate genuine storefront login proofs. A forged
profile can still be obtained through those legacy mechanisms. Legacy match APIs
are unchanged and do not receive the new room authorization checks.

Before remote testing/public hosting, implement real authentication, private
server signing keys, TLS, request/join-attempt limits and a reviewed hosting model.
Join codes are not authentication or transport credentials. The 8 KiB JSON limit,
1,000-room bound and heartbeat leases are not substitutes for those protections.

## Client compatibility descriptor

Create and Join require this exact shape (example tags, not certified compatibility):

```json
{
  "protocolVersion": 1,
  "gameBuild": "3.280.0.0/10545744",
  "assetSet": "ghost-assets-test-v1",
  "transportProtocol": "unimplemented-v1"
}
```

All four values must match the host. Protocol version must be `1`; other tags are
1–128 ASCII characters (`A-Z`, `a-z`, digits, `.`, `_`, `:`, `/`, `+`, `-`), starting
with a letter or digit. These are client assertions, not server attestation.

Future clients must verify their supported executable hash/native layouts and
asset payload before advertising readiness. Use a common engine/build tag across
storefronts, **not an executable hash**, since Steam and Epic executables can differ.
Define the asset tag from a common payload/compatibility revision, not merely the
Epic experiment's renamed manifest. A real transport revision must replace the
test-only `unimplemented-v1` tag before gameplay is attempted.

Only Miami and Santa Fortuna are admitted. `GetCapabilities` returns their actual
LocalGhost contract and preset IDs. This is not proof of native loading or ownership;
each client must own and have installed the required licensed content.

## HTTP contract

Base path: `/authentication/api/userchannel/GhostLobbyService/`

Every operation is a JSON **POST** with `Authorization: Bearer <LocalGhost token>`
(the game's `X-GPS-Authorization` header is also supported). Player identity and
storefront come exclusively from the verified token, never request JSON. Requests
must contain exactly the fields below; unknown/missing fields are rejected.

| Operation | JSON fields | Result / rules |
| --- | --- | --- |
| `GetCapabilities` | `{}` | Protocol, maps, capacity, lease/heartbeat intervals, `nativeIntegration:false`, `gameplayTransport:false` |
| `Create` | `mapId`, `client` | Host a room; return snapshot |
| `Join` | `joinCode`, `client` | Join a room; return snapshot; code is case-insensitive |
| `GetLobby` | `lobbyId`, `memberId` | Snapshot; does **not** renew lease |
| `Heartbeat` | `lobbyId`, `memberId` | Renew only caller's lease; return snapshot |
| `SetMap` | `lobbyId`, `memberId`, `revision`, `mapId` | Host only; changing map resets both ready flags |
| `SetReady` | `lobbyId`, `memberId`, `revision`, `ready` | Boolean; changes only caller's ready flag |
| `Start` | `lobbyId`, `memberId`, `revision` | Host only; requires two active, ready players; returns **launch intent** |
| `Leave` | `lobbyId`, `memberId` | `{ "closed": true/false }`; see lifecycle below |

`mapId` is `miami` or `santa-fortuna`. IDs are UUID strings; use server-returned
values verbatim. `memberId` is a private handle for this particular membership,
not an authentication token. A rejoin after leave/expiry issues a fresh handle;
old requests cannot act on the replacement membership.

Snapshot fields:

| Field | Meaning |
| --- | --- |
| `protocolVersion` | `1` |
| `lobbyId`, `joinCode` | Room UUID and eight-character invitation code |
| `revision` | Positive integer; increments on membership, map, ready or launch changes |
| `phase` | `waiting`, `ready`, or `launch_requested` |
| `hostId` | Host's profile UUID |
| `map` | `{ id, contractId, presetId }` |
| `client` | Host's compatibility descriptor |
| `self` | `{ playerId, memberId }` for the authenticated caller only |
| `players` | Array of `{ playerId, platform, role, ready, expiresAt }`; no peer's membership handle |
| `launch` | `null` or `{ launchId, requestRevision, contractId, playerIds, requestedAt }` |

Time values are server Unix milliseconds. Heartbeats change `expiresAt` but not
`revision`. Do not use revision alone to deduplicate full lease snapshots.

**`launchId` is NOT a legacy `MatchId`, native session, relay allocation or transport
key.** Start does not call `MultiplayerService/RegisterToMatch`, load a map, emit
legacy push messages, or change the game's lobby state. There is deliberately no
`playing` status: no native client can yet confirm a working gameplay session.

### Lifecycle and retries

1. Create/Join returns caller's membership handle. Exact retries while that
   membership remains active return the same room/handle and renew its lease.
   This is not durable idempotency after a room closes or the process restarts.
2. Send Heartbeat every 5 seconds; each caller has a 30-second lease. Ordinary
   reads and mutations other than Create/Join/Heartbeat do not renew it.
3. Poll Heartbeat snapshots or GetLobby. Membership changes and map changes clear
   **all** ready flags. A client must recheck the current map/resources before ready.
4. SetMap, SetReady and Start use the last snapshot's revision. On
   `STALE_REVISION`, poll and reconsider the action; never blindly replay a ready
   confirmation made for an old map/peer.
5. Both ready players produce `phase:ready`. Host Start freezes the room in
   `launch_requested`. Retrying with the original request revision or resulting
   room revision returns the same launch ID. Map/ready changes are now blocked.
6. Host leave/expiry closes the room: no host migration. Guest leave/expiry in an
   open room removes the guest and resets host readiness, allowing a new guest.
   **Either member leaving/expiring after launch intent closes the entire room**;
   never replace an opponent inside an existing launch.
7. Closed, expired or restarted rooms return 404. Stop polling, tear down any
   future local session/transport, discard handles and return to host/join UI.
   Leave retries after closure likewise return 404, which clients may treat as
   already left. Rejoining an open room requires a new ready confirmation.

Expiry is enforced before every room access/creation/join and during the existing
server cleanup pass. No operation can revive a membership at or past its deadline.
All state transitions are synchronous within one process, including capacity and
ownership checks; no asynchronous read/write gap can admit a third player.

### Errors

Success status is 200. API-generated errors have shape:

```json
{ "error": { "code": "STALE_REVISION", "message": "Poll the lobby and retry against its current revision." } }
```

| Status | Typical codes |
| --- | --- |
| 400 | `INVALID_REQUEST`, `INVALID_JSON`, `INVALID_CLIENT`, `INVALID_CODE`, `UNSUPPORTED_MAP` |
| 401 | `AUTH_REQUIRED`, `INVALID_TOKEN` |
| 403 | `WOA_REQUIRED`, `UNSUPPORTED_IDENTITY`, `NOT_A_MEMBER`, `HOST_ONLY` |
| 404 | `LOBBY_NOT_FOUND` |
| 409 | `PROTOCOL_MISMATCH`, `CLIENT_MISMATCH`, `ALREADY_IN_LOBBY`, `LOBBY_FULL`, `STALE_REVISION`, `NOT_READY`, `LOBBY_LOCKED` |
| 413 | `INVALID_JSON` (body exceeds 8 KiB) |
| 503 | `LOBBY_LIMIT`, `CODE_UNAVAILABLE` |

LocalGhost's outer legacy middleware may return an empty 401 body before this
router runs. Clients must handle HTTP status even when no structured JSON exists.

## Native bridge interface: next checkpoint, not implemented

The proposed client boundary is the same on Steam and Epic:

- **UI → lobby client:** Host(map), Join(code), SelectMap(map), Ready(bool), Start,
  Cancel/Leave. A standalone companion implements this control-plane UI first,
  without activating SDK hooks; an in-game host/join panel remains future work.
- **Lobby client → UI:** snapshots, actionable compatibility/errors, disconnect.
  Keep bearer tokens and membership handles out of UI logs and peer messages.
- **Lobby client → engine adapter:** a single launch intent containing the room,
  launch ID, contract and two profile IDs. Dispatch it once per launch ID on the
  game thread; HTTP/JSON work belongs on a worker, not in a game-frame hook.
- **Engine adapter → diagnostics:** native lobby state transitions, resource
  availability, local host creation, contract load and teardown/failure. The
  original Epic build must first demonstrate a usable engine session lifecycle.
- **Transport adapter → engine adapter:** opponent movement/appearance and the
  Ghost gameplay events in a shared wire protocol. Transport allocation, keys,
  handshake, sequencing and replication remain to be designed/implemented.

Do not activate the SDK's old fixed-address lobby experiment wholesale. Initially
use exact executable/version checks and diagnostic-only hooks to establish whether
the surviving local-host path can reach a valid lobby and load Miami. A successful
local-host transition is still not an opponent, gameplay transport or crossplay.

Keep room state, native session state and transport state separate. A future
launch must fail closed if resources, native hooks or transport are unavailable;
do not mark peers connected solely because they share an HTTP room. The worlds'
AI/simulation stay separate; replicating all entities as ordinary co-op is not
Ghost Mode. Legacy match registration integration needs an explicit authorized
mapping, not feeding `launchId` into today's permissive legacy APIs.

## Validation

From the workspace root, with the approved Node 22 runtime selected:

```powershell
node --test tools/ghost-lobby-store.test.cjs tools/localghost-smoke.cjs
```

Unit tests inject clock/code/ID sources for deterministic expiry and collision
checks. Integration tests use synthetic Steam/Epic accounts, actual server-issued
JWTs, scratch profiles, and a separate ephemeral loopback listener. The suite
also retains checks for legacy login/resources/match data and push behavior.
No genuine credentials, game instances, deployed packages or live user profiles
are modified. Two licensed game instances are still required for gameplay and
Steam ↔ Epic acceptance testing.
