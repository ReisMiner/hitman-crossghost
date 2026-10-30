import http from 'node:http';
import dgram from 'node:dgram';
import { randomBytes, randomInt } from 'node:crypto';
import { fieldsDecode, fieldsEncode, Kind, packetDecode, packetEncode, ReplayWindow } from './protocol.mjs';

// Five digits keep codes easy to share. Joins are rate-limited per IP and a room
// accepts one guest, so guessing is bounded; maxRooms stays far below 100000.
const roomCode = () => String(randomInt(100000)).padStart(5, '0');
export function createRoomService({ backendUrl = '', host = '127.0.0.1', udpPort = 30002,
    udpHost = host, advertisedUdpPort, maxRooms = 1000, leaseMs = 30000, maxAgeMs = 6 * 3600000, now = Date.now } = {}) {
    if (backendUrl) {
        const url = new URL(backendUrl);
        if (backendUrl.length > 512 || !['https:', 'http:'].includes(url.protocol) || url.username || url.password || url.pathname !== '/' || url.search || url.hash)
            throw new Error('backendUrl must be an HTTP(S) origin');
    }
    const rooms = new Map(), codes = new Map(), clients = new Map();
    const udp = dgram.createSocket('udp4');
    let boundUdpPort = 0;
    let closed = false;
    function member() {
        return { token: randomBytes(32), endpoint: null, lastSeen: now(), replay: new ReplayWindow(),
            sequence: 0n, budget: 4 * 1024 * 1024, packets: 2000, budgetAt: now() };
    }
    function send(room, target, role, kind, channel = 0, payload) {
        if (!target?.endpoint) return;
        const bytes = packetEncode(room.id, target.token, role, kind, channel, ++target.sequence, payload);
        udp.send(bytes, target.endpoint.port, target.endpoint.address, () => {});
    }
    function remove(room) {
        send(room, room.host, 0, Kind.gone); send(room, room.guest, 1, Kind.gone);
        rooms.delete(room.id); codes.delete(room.code);
    }
    function sweep() {
        const time = now();
        for (const room of rooms.values()) {
            if (time - room.host.lastSeen > leaseMs || time - room.created > maxAgeMs) remove(room);
            else if (room.guest && time - room.guest.lastSeen > leaseMs) {
                send(room, room.guest, 1, Kind.gone); room.guest = null;
            }
        }
        for (const [key, value] of clients) if (time - value.start > 60000) clients.delete(key);
    }
    const timer = setInterval(sweep, 1000); timer.unref();
    function reply(res, status, fields) {
        res.writeHead(status, { 'Content-Type': 'application/octet-stream', 'Cache-Control': 'no-store' });
        res.end(fieldsEncode(fields));
    }
    function credentials(room, peer, role) {
        return { RoomId: room.id, RoomCode: room.code, Token: peer.token.toString('hex'), Role: String(role),
            UdpPort: String(advertisedUdpPort ?? boundUdpPort), BackendUrl: backendUrl, Protocol: '1' };
    }
    async function handleRequest(req, res) {
        if (req.method === 'GET' && req.url === '/healthz') { res.writeHead(200); res.end('ok'); return; }
        if (req.method === 'GET' && req.url === '/v1/config') { reply(res, 200, { BackendUrl: backendUrl, Protocol: '1' }); return; }
        if (req.method !== 'POST' || !['/v1/rooms/create', '/v1/rooms/join', '/v1/rooms/leave'].includes(req.url)) {
            reply(res, 404, { Error: 'Unknown room operation' }); return;
        }
        // Express supplies req.ip using LocalGhost's configured proxy policy.
        // Standalone tests use the socket peer; never parse raw forwarded headers.
        const ip = req.ip ?? req.socket.remoteAddress;
        let rate = clients.get(ip);
        if (!rate || now() - rate.start > 60000) {
            if (clients.size >= 10000) { reply(res, 503, { Error: 'Service busy' }); return; }
            clients.set(ip, rate = { start: now(), count: 0 });
        }
        if (++rate.count > 120) { reply(res, 429, { Error: 'Too many requests; try again shortly' }); req.resume(); return; }
        try {
            let size = 0; const chunks = [];
            for await (const chunk of req) {
                size += chunk.length;
                if (size > 4096) { reply(res, 413, { Error: 'Request too large' }); return; }
                chunks.push(chunk);
            }
            const fields = fieldsDecode(Buffer.concat(chunks));
            sweep();
            if (req.url === '/v1/rooms/leave') {
                const room = rooms.get(fields.RoomId);
                if (room && fields.Token === room.host.token.toString('hex')) remove(room);
                else if (room?.guest && fields.Token === room.guest.token.toString('hex')) {
                    send(room, room.guest, 1, Kind.gone); room.guest = null;
                }
                reply(res, 200, { Status: 'Closed' }); return;
            }
            if (fields.Protocol !== '1' || fields.Build !== '3.290.1.0') {
                reply(res, 409, { Error: 'Incompatible client protocol or game build' }); return;
            }
            if (req.url === '/v1/rooms/create') {
                if (fields.Store !== 'epic' && fields.Store !== 'steam') { reply(res, 409, { Error: 'Unsupported host storefront' }); return; }
                if (rooms.size >= maxRooms) { reply(res, 503, { Error: 'Service is full' }); return; }
                let code; do { code = roomCode(); } while (codes.has(code));
                const room = { id: randomBytes(16).toString('hex'), code, host: member(), guest: null, created: now() };
                rooms.set(room.id, room); codes.set(code, room);
                reply(res, 201, credentials(room, room.host, 0)); return;
            }
            if (fields.Store !== 'epic' && fields.Store !== 'steam') { reply(res, 409, { Error: 'Unsupported guest storefront' }); return; }
            const code = String(fields.RoomCode ?? '').replace(/[-\s]/g, '');
            const room = /^\d{5}$/.test(code) ? codes.get(code) : null;
            if (!room || !room.host.endpoint) { reply(res, 404, { Error: 'Room not found or host is not connected' }); return; }
            if (room.guest) { reply(res, 409, { Error: 'Room is full' }); return; }
            room.guest = member();
            reply(res, 200, credentials(room, room.guest, 1));
        } catch {
            if (!res.headersSent && !res.destroyed) reply(res, 400, { Error: 'Invalid room request' });
        }
    }
    udp.on('message', (bytes, remote) => {
        if (bytes.length < 64 || bytes.length > 65507 || bytes[5] > 1) return;
        const room = rooms.get(bytes.toString('hex', 8, 24));
        if (!room) return;
        if (now() - room.host.lastSeen > leaseMs || now() - room.created > maxAgeMs) { remove(room); return; }
        if (room.guest && now() - room.guest.lastSeen > leaseMs) { send(room, room.guest, 1, Kind.gone); room.guest = null; }
        const role = bytes[5], peer = role === 0 ? room.host : room.guest;
        if (!peer) return;
        const packet = packetDecode(bytes, peer.token);
        if (!packet || (packet.kind !== Kind.hello && packet.kind !== Kind.up) ||
            (packet.kind === Kind.hello && (packet.channel || packet.payload.length)) ||
            (packet.kind === Kind.up && (packet.channel < 1 || packet.channel > 3 || !packet.payload.length))) return;
        const previousHigh = peer.replay.high;
        if (!peer.replay.accept(packet.sequence)) return;
        const elapsed = Math.max(0, now() - peer.budgetAt) / 1000;
        peer.budgetAt = now();
        peer.budget = Math.min(4 * 1024 * 1024, peer.budget + elapsed * 4 * 1024 * 1024);
        peer.packets = Math.min(2000, peer.packets + elapsed * 2000);
        if (peer.budget < bytes.length || peer.packets < 1) return;
        peer.budget -= bytes.length; peer.packets--;
        peer.lastSeen = now();
        if (packet.sequence > previousHigh) peer.endpoint = { address: remote.address, port: remote.port };
        if (packet.kind === Kind.hello) send(room, peer, role, Kind.ack);
        else send(room, role === 0 ? room.guest : room.host, 1 - role, Kind.down, packet.channel, packet.payload);
    });
    // send() callbacks consume individual UDP errors; surface socket-level errors.
    udp.on('error', err => { console.error('UDP service error:', err.message); });
    return {
        handleRequest,
        async listen() {
            try {
                await new Promise((resolve, reject) => {
                    const failed = error => reject(error);
                    udp.once('error', failed);
                    udp.bind(udpPort, udpHost, () => { udp.off('error', failed); resolve(); });
                });
            } catch (error) {
                clearInterval(timer);
                throw error;
            }
            boundUdpPort = udp.address().port;
            return { udpPort: boundUdpPort };
        },
        async close() {
            if (closed) return;
            closed = true;
            clearInterval(timer);
            for (const room of rooms.values()) remove(room);
            await new Promise(resolve => {
                try { udp.close(resolve); } catch { resolve(); }
            });
        },
        sweep,
    };
}

// Standalone HTTP adapter for protocol/native integration tests. Production
// mounts handleRequest directly in LocalGhost's existing Express application.
export function createService({ host = '127.0.0.1', httpHost = host, httpPort = 8787, ...options } = {}) {
    const rooms = createRoomService({ host, ...options });
    const server = http.createServer((req, res) => {
        rooms.handleRequest(req, res).catch(() => { res.writeHead(500); res.end(); });
    });
    server.requestTimeout = 10000; server.headersTimeout = 10000; server.maxHeadersCount = 32;
    return {
        async listen() {
            try {
                const relay = await rooms.listen();
                await new Promise((resolve, reject) => { server.once('error', reject); server.listen(httpPort, httpHost, resolve); });
                return { httpPort: server.address().port, ...relay };
            } catch (error) { await rooms.close(); throw error; }
        },
        async close() {
            await Promise.all([new Promise(resolve => server.close(resolve)), rooms.close()]);
        },
        sweep: rooms.sweep,
    };
}
