import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import http from 'node:http';
import dgram from 'node:dgram';
import { fileURLToPath } from 'node:url';
import { fieldsEncode, fieldsDecode, packetEncode, packetDecode, Kind } from '../../components/ghostRooms/protocol.mjs';

const publicOrigin = 'https://hitmanghost.ramon.cc';

async function launch(t, overrides = {}) {
    const child = spawn(process.execPath, ['serb.js'], {
        cwd: fileURLToPath(new URL('../../', import.meta.url)),
        env: { ...process.env, NODE_ENV: 'production', PUBLIC_ORIGIN: publicOrigin,
            BIND_HOST: '127.0.0.1', PORT: '0', UDP_HOST: '127.0.0.1', UDP_PORT: '0',
            PUBLIC_UDP_PORT: '', TRUST_PROXY: 'none', ...overrides },
        stdio: ['ignore', 'pipe', 'pipe'],
    });
    const exited = once(child, 'exit');
    t.after(async () => {
        if (child.exitCode === null && child.signalCode === null) child.kill();
        await exited;
    });
    return new Promise((resolve, reject) => {
        let output = '', pending = '';
        const deadline = setTimeout(() => {
            child.kill(); reject(new Error(`LocalGhost startup timed out: ${output}`));
        }, 8000);
        child.once('error', error => { clearTimeout(deadline); reject(error); });
        child.once('exit', code => {
            clearTimeout(deadline); reject(new Error(`LocalGhost exited (${code}): ${output}`));
        });
        child.stderr.on('data', bytes => { output += bytes; });
        child.stdout.on('data', bytes => {
            output += bytes; pending += bytes;
            const lines = pending.split('\n'); pending = lines.pop();
            for (const line of lines) {
                if (!line.startsWith('{')) continue;
                const value = JSON.parse(line);
                if (value.event === 'localghost-ready') {
                    clearTimeout(deadline); resolve(value);
                }
            }
        });
    });
}

async function peer(t) {
    const socket = dgram.createSocket('udp4');
    socket.bind(0, '127.0.0.1'); await once(socket, 'listening');
    t.after(() => new Promise(resolve => socket.close(resolve)));
    return socket;
}

const receive = async socket => (await once(socket, 'message', { signal: AbortSignal.timeout(2000) }))[0];
const pack = (credentials, role, kind, sequence, payload) => packetEncode(credentials.RoomId,
    Buffer.from(credentials.Token, 'hex'), role, kind, kind === Kind.up ? 3 : 0, sequence, payload);
const unpack = (bytes, credentials) => packetDecode(bytes, Buffer.from(credentials.Token, 'hex'));

test('one LocalGhost listener serves game config/auth and room-code UDP transport', { timeout: 15000 }, async t => {
    const address = await launch(t);
    assert.equal(address.publicOrigin, publicOrigin);
    const origin = `http://127.0.0.1:${address.httpPort}`;
    const request = (path, options) => fetch(origin + path, { signal: AbortSignal.timeout(3000), ...options });
    assert.equal(await (await request('/healthz')).text(), 'ok');

    // A proxy's internal Host or scheme must not leak into public game URLs.
    const config = await new Promise((resolve, reject) => {
        const req = http.get(origin + '/config/pc-prod/8_2_0', {
            headers: { Host: 'internal-upstream:47047', 'X-Forwarded-Proto': 'http', 'X-Forwarded-Host': 'wrong.example' },
        }, res => {
            let body = ''; res.on('data', bytes => { body += bytes; });
            res.on('end', () => {
                try { assert.equal(res.statusCode, 200); resolve(JSON.parse(body)); } catch (error) { reject(error); }
            });
        });
        req.setTimeout(3000, () => req.destroy(new Error('config timeout'))); req.on('error', reject);
    });
    const server = config.Versions[0].SERVER_VER;
    assert.equal(server.Authentication.AuthenticationHost, publicOrigin);
    assert.equal(server.Resources.ResourcesServicePath, publicOrigin + '/files');
    assert.equal(server.Configuration.Url, publicOrigin + '/files/onlineconfig.json');
    assert.equal((await request('/files/onlineconfig.json')).status, 200);

    // Exercise the existing token middleware and blob URL path without writing profiles.
    const claim = { unique_name: 'fadb923c-e6bb-4283-a537-eb4d1150262e', pis: '1659040', platform: 'steam',
        userid: '76561198000000000', exp: 9999999999, nbf: 1 };
    const token = [ { alg: 'none', typ: 'JWT' }, claim ].map(value => Buffer.from(JSON.stringify(value)).toString('base64url')).join('.') + '.';
    const init = await request('/authentication/api/configuration/Init?configName=pc-prod', {
        headers: { Authorization: `Bearer ${token}` },
    });
    assert.equal(init.status, 200);
    assert.equal((await init.json()).blobconfig.bloburl, publicOrigin + '/resources-8-2/');

    const roomConfig = fieldsDecode(Buffer.from(await (await request('/v1/config')).arrayBuffer()));
    assert.equal(roomConfig.BackendUrl, publicOrigin);
    async function post(operation, values) {
        const response = await request('/v1/rooms/' + operation, {
            method: 'POST', headers: { 'Content-Type': 'application/octet-stream' },
            body: fieldsEncode({ Protocol: '1', Build: '3.290.1.0', ...values }),
        });
        return { status: response.status, fields: fieldsDecode(Buffer.from(await response.arrayBuffer())) };
    }
    const host = await peer(t), guest = await peer(t);
    const created = await post('create', { Store: 'epic' });
    assert.equal(created.status, 201);
    const h = created.fields;
    assert.equal(h.BackendUrl, publicOrigin);
    assert.equal(Number(h.UdpPort), address.udpPort);
    const hostAck = receive(host);
    host.send(pack(h, 0, Kind.hello, 1n), address.udpPort, '127.0.0.1');
    assert.equal(unpack(await hostAck, h).kind, Kind.ack);

    const joined = await post('join', { Store: 'steam', RoomCode: h.RoomCode });
    assert.equal(joined.status, 200);
    const g = joined.fields;
    const guestAck = receive(guest);
    guest.send(pack(g, 1, Kind.hello, 1n), address.udpPort, '127.0.0.1');
    assert.equal(unpack(await guestAck, g).kind, Kind.ack);
    const payload = Buffer.from([0x05, 0xff, 0, 0xad]);
    const forwarded = receive(host);
    guest.send(pack(g, 1, Kind.up, 2n, payload), address.udpPort, '127.0.0.1');
    assert.deepEqual(unpack(await forwarded, h).payload, payload);

    assert.equal((await post('leave', g)).status, 200);
    assert.equal((await post('join', { Store: 'steam', RoomCode: h.RoomCode })).status, 200);
    assert.equal((await post('leave', h)).status, 200);
    assert.equal((await post('join', { Store: 'steam', RoomCode: h.RoomCode })).status, 404);
    assert.equal((await request('/config/pc-prod/8_2_0')).status, 200, 'game API remains available after room teardown');
});

test('LocalGhost startup fails when its UDP relay cannot bind', { timeout: 15000 }, async t => {
    const occupied = await peer(t);
    await assert.rejects(launch(t, { UDP_PORT: String(occupied.address().port) }), /EADDRINUSE/);
});
