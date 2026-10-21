// Store-independent lobby control plane. No native sessions or gameplay transport.
const { randomInt, randomUUID } = require('crypto');

const PROTOCOL_VERSION = 1;
const CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const MAPS = Object.freeze([
    Object.freeze({ id: 'miami', contractId: 'c854d9c7-8991-42db-a2e4-3cf93e645e81', presetId: 'd72d7cc9-ee26-4c7d-857a-75abdc9ccb61' }),
    Object.freeze({ id: 'santa-fortuna', contractId: 'd5ad70f4-7dbb-4b2b-b183-061778a51468', presetId: 'f5d7c89d-1428-42b2-b3cb-acaa036e246d' }),
]);

class LobbyError extends Error {
    constructor(status, code, message) {
        super(message);
        this.status = status;
        this.code = code;
    }
}

function requireCondition(condition, status, code, message) {
    if (!condition) throw new LobbyError(status, code, message);
}

function validateFields(value, keys) {
    requireCondition(value && typeof value === 'object' && !Array.isArray(value) &&
        Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key)),
    400, 'INVALID_REQUEST', `Expected exactly: ${keys.join(', ') || 'an empty object'}.`);
}

function validateClient(client) {
    validateFields(client, ['protocolVersion', 'gameBuild', 'assetSet', 'transportProtocol']);
    requireCondition(client.protocolVersion === PROTOCOL_VERSION, 409, 'PROTOCOL_MISMATCH', 'Unsupported lobby protocol.');
    for (const field of ['gameBuild', 'assetSet', 'transportProtocol']) {
        requireCondition(typeof client[field] === 'string' && /^[A-Za-z0-9][A-Za-z0-9._:/+\-]{0,127}$/.test(client[field]),
            400, 'INVALID_CLIENT', `Invalid client ${field} tag.`);
    }
}

function makeJoinCode() {
    return Array.from({ length: 8 }, () => CODE_ALPHABET[randomInt(CODE_ALPHABET.length)]).join('');
}

class GhostLobbyStore {
    constructor({ now = Date.now, makeId = randomUUID, makeCode = makeJoinCode, leaseMs = 30000, maxLobbies = 1000 } = {}) {
        this.now = now;
        this.makeId = makeId;
        this.makeCode = makeCode;
        this.leaseMs = leaseMs;
        this.maxLobbies = maxLobbies;
        this.lobbies = new Map();
        this.codes = new Map();
        this.memberships = new Map();
    }

    capabilities() {
        return {
            protocolVersion: PROTOCOL_VERSION,
            capacity: 2,
            leaseMs: this.leaseMs,
            heartbeatIntervalMs: Math.floor(this.leaseMs / 6),
            maps: MAPS.map(map => ({ ...map })),
            nativeIntegration: false,
            gameplayTransport: false,
        };
    }

    getCapabilities(identity, body) {
        this.actor(identity);
        validateFields(body, []);
        return this.capabilities();
    }

    actor(identity) {
        requireCondition(identity && typeof identity.playerId === 'string' && UUID.test(identity.playerId) &&
            ['steam', 'epic'].includes(identity.platform), 403, 'UNSUPPORTED_IDENTITY', 'A WOA Steam or Epic profile is required.');
        return { playerId: identity.playerId.toLowerCase(), platform: identity.platform };
    }

    map(mapId) {
        const map = MAPS.find(map => map.id === mapId);
        requireCondition(map, 400, 'UNSUPPORTED_MAP', 'This server has no supported Ghost contract for that map.');
        return map;
    }

    compatible(lobby, client) {
        requireCondition(Object.keys(lobby.client).every(key => lobby.client[key] === client[key]),
            409, 'CLIENT_MISMATCH', 'Game build, assets and transport protocol must match the host.');
    }

    addMember(lobby, actor) {
        const member = { ...actor, memberId: this.makeId(), ready: false, expiresAt: this.now() + this.leaseMs };
        lobby.players.set(actor.playerId, member);
        this.memberships.set(actor.playerId, lobby.id);
        return member;
    }

    resetReadiness(lobby) {
        for (const member of lobby.players.values()) member.ready = false;
        lobby.phase = 'waiting';
    }

    close(lobby) {
        for (const playerId of lobby.players.keys()) this.memberships.delete(playerId);
        this.codes.delete(lobby.code);
        this.lobbies.delete(lobby.id);
    }

    cleanup() {
        const now = this.now();
        for (const lobby of this.lobbies.values()) {
            if (lobby.players.get(lobby.hostId).expiresAt <= now ||
                (lobby.phase === 'launch_requested' && [...lobby.players.values()].some(player => player.expiresAt <= now))) {
                this.close(lobby);
                continue;
            }
            for (const player of lobby.players.values()) {
                if (player.expiresAt <= now) {
                    lobby.players.delete(player.playerId);
                    this.memberships.delete(player.playerId);
                    this.resetReadiness(lobby);
                    lobby.revision++;
                }
            }
        }
    }

    member(lobbyId, memberId, actor) {
        requireCondition(typeof lobbyId === 'string' && UUID.test(lobbyId) && typeof memberId === 'string' && UUID.test(memberId),
            400, 'INVALID_REQUEST', 'lobbyId and memberId must be UUIDs.');
        this.cleanup();
        const lobby = this.lobbies.get(lobbyId);
        requireCondition(lobby, 404, 'LOBBY_NOT_FOUND', 'Lobby closed, expired or unknown.');
        const member = lobby.players.get(actor.playerId);
        requireCondition(member && member.memberId === memberId, 403, 'NOT_A_MEMBER', 'This membership is not active.');
        return { lobby, member };
    }

    host(lobby, actor) {
        requireCondition(lobby.hostId === actor.playerId, 403, 'HOST_ONLY', 'Only the host can perform this operation.');
    }

    revision(lobby, revision) {
        requireCondition(Number.isSafeInteger(revision) && revision >= 1, 400, 'INVALID_REQUEST', 'revision must be a positive integer.');
        requireCondition(lobby.revision === revision, 409, 'STALE_REVISION', 'Poll the lobby and retry against its current revision.');
    }

    open(lobby) {
        requireCondition(lobby.phase !== 'launch_requested', 409, 'LOBBY_LOCKED', 'A launch was already requested. Leave to create another lobby.');
    }

    snapshot(lobby, actor) {
        return {
            protocolVersion: PROTOCOL_VERSION,
            lobbyId: lobby.id,
            joinCode: lobby.code,
            revision: lobby.revision,
            phase: lobby.phase,
            hostId: lobby.hostId,
            map: { ...this.map(lobby.mapId) },
            client: { ...lobby.client },
            self: { playerId: actor.playerId, memberId: lobby.players.get(actor.playerId).memberId },
            players: [...lobby.players.values()].map(player => ({
                playerId: player.playerId, platform: player.platform, ready: player.ready,
                role: player.playerId === lobby.hostId ? 'host' : 'guest', expiresAt: player.expiresAt,
            })),
            launch: lobby.launch ? { ...lobby.launch, playerIds: [...lobby.launch.playerIds] } : null,
        };
    }

    create(identity, body) {
        const actor = this.actor(identity);
        validateFields(body, ['mapId', 'client']);
        this.map(body.mapId);
        validateClient(body.client);
        this.cleanup();
        const existing = this.lobbies.get(this.memberships.get(actor.playerId));
        if (existing) {
            requireCondition(existing.hostId === actor.playerId && existing.mapId === body.mapId,
                409, 'ALREADY_IN_LOBBY', 'Leave the current lobby before creating another.');
            this.compatible(existing, body.client);
            existing.players.get(actor.playerId).expiresAt = this.now() + this.leaseMs;
            return this.snapshot(existing, actor);
        }
        requireCondition(this.lobbies.size < this.maxLobbies, 503, 'LOBBY_LIMIT', 'The server lobby limit was reached.');
        let code;
        for (let attempt = 0; attempt < 16; attempt++) {
            const candidate = this.makeCode();
            if (!this.codes.has(candidate)) { code = candidate; break; }
        }
        requireCondition(code, 503, 'CODE_UNAVAILABLE', 'Could not allocate a join code. Retry later.');
        const lobby = {
            id: this.makeId(), code, hostId: actor.playerId, mapId: body.mapId,
            client: { ...body.client }, players: new Map(), phase: 'waiting', revision: 1, launch: null,
        };
        this.addMember(lobby, actor);
        this.lobbies.set(lobby.id, lobby);
        this.codes.set(code, lobby.id);
        return this.snapshot(lobby, actor);
    }

    join(identity, body) {
        const actor = this.actor(identity);
        validateFields(body, ['joinCode', 'client']);
        validateClient(body.client);
        requireCondition(typeof body.joinCode === 'string' && /^[A-HJ-NP-Z2-9]{8}$/i.test(body.joinCode),
            400, 'INVALID_CODE', 'Expected an eight-character join code.');
        this.cleanup();
        const lobby = this.lobbies.get(this.codes.get(body.joinCode.toUpperCase()));
        requireCondition(lobby, 404, 'LOBBY_NOT_FOUND', 'Lobby closed, expired or unknown.');
        this.compatible(lobby, body.client);
        const existing = this.memberships.get(actor.playerId);
        requireCondition(!existing || existing === lobby.id, 409, 'ALREADY_IN_LOBBY', 'Leave the current lobby before joining another.');
        if (lobby.players.has(actor.playerId)) {
            lobby.players.get(actor.playerId).expiresAt = this.now() + this.leaseMs;
            return this.snapshot(lobby, actor);
        }
        this.open(lobby);
        requireCondition(lobby.players.size < 2, 409, 'LOBBY_FULL', 'This lobby already has two players.');
        this.addMember(lobby, actor);
        this.resetReadiness(lobby);
        lobby.revision++;
        return this.snapshot(lobby, actor);
    }

    get(identity, body, heartbeat = false) {
        const actor = this.actor(identity);
        validateFields(body, ['lobbyId', 'memberId']);
        const { lobby, member } = this.member(body.lobbyId, body.memberId, actor);
        if (heartbeat) member.expiresAt = this.now() + this.leaseMs;
        return this.snapshot(lobby, actor);
    }

    setMap(identity, body) {
        const actor = this.actor(identity);
        validateFields(body, ['lobbyId', 'memberId', 'revision', 'mapId']);
        this.map(body.mapId);
        const { lobby } = this.member(body.lobbyId, body.memberId, actor);
        this.host(lobby, actor);
        this.open(lobby);
        this.revision(lobby, body.revision);
        if (lobby.mapId !== body.mapId) {
            lobby.mapId = body.mapId;
            this.resetReadiness(lobby);
            lobby.revision++;
        }
        return this.snapshot(lobby, actor);
    }

    setReady(identity, body) {
        const actor = this.actor(identity);
        validateFields(body, ['lobbyId', 'memberId', 'revision', 'ready']);
        requireCondition(typeof body.ready === 'boolean', 400, 'INVALID_REQUEST', 'ready must be a boolean.');
        const { lobby, member } = this.member(body.lobbyId, body.memberId, actor);
        this.open(lobby);
        this.revision(lobby, body.revision);
        if (member.ready !== body.ready) {
            member.ready = body.ready;
            lobby.phase = lobby.players.size === 2 && [...lobby.players.values()].every(player => player.ready) ? 'ready' : 'waiting';
            lobby.revision++;
        }
        return this.snapshot(lobby, actor);
    }

    start(identity, body) {
        const actor = this.actor(identity);
        validateFields(body, ['lobbyId', 'memberId', 'revision']);
        const { lobby } = this.member(body.lobbyId, body.memberId, actor);
        this.host(lobby, actor);
        // An exact retry of a successful Start returns the same launch ID.
        if (lobby.launch && (body.revision === lobby.launch.requestRevision || body.revision === lobby.revision)) {
            return this.snapshot(lobby, actor);
        }
        this.open(lobby);
        this.revision(lobby, body.revision);
        requireCondition(lobby.phase === 'ready', 409, 'NOT_READY', 'Two active, ready players are required.');
        lobby.launch = {
            launchId: this.makeId(), requestRevision: body.revision,
            contractId: this.map(lobby.mapId).contractId, playerIds: [...lobby.players.keys()], requestedAt: this.now(),
        };
        lobby.phase = 'launch_requested';
        lobby.revision++;
        return this.snapshot(lobby, actor);
    }

    leave(identity, body) {
        const actor = this.actor(identity);
        validateFields(body, ['lobbyId', 'memberId']);
        const { lobby } = this.member(body.lobbyId, body.memberId, actor);
        if (lobby.hostId === actor.playerId || lobby.phase === 'launch_requested') {
            this.close(lobby);
            return { closed: true };
        }
        lobby.players.delete(actor.playerId);
        this.memberships.delete(actor.playerId);
        this.resetReadiness(lobby);
        lobby.revision++;
        return { closed: false };
    }
}

module.exports = { GhostLobbyStore, LobbyError };
