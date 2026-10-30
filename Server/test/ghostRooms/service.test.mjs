import { test } from 'node:test';
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { createService } from '../../components/ghostRooms/service.mjs';
import { fieldsEncode, fieldsDecode, packetEncode, packetDecode, Kind, ReplayWindow } from '../../components/ghostRooms/protocol.mjs';

async function peer() {
    const socket = dgram.createSocket('udp4');
    const queue = []; let waiter;
    socket.on('message', bytes => { if (waiter) { const done = waiter; waiter = null; done(bytes); } else queue.push(bytes); });
    await new Promise(resolve => socket.bind(0, '127.0.0.1', resolve));
    return {
        send(bytes, port) { socket.send(bytes, port, '127.0.0.1'); },
        receive(ms = 500) {
            if (queue.length) return Promise.resolve(queue.shift());
            return new Promise(resolve => {
                const timeout = setTimeout(() => { waiter = null; resolve(null); }, ms);
                waiter = bytes => { clearTimeout(timeout); resolve(bytes); };
            });
        },
        close() { socket.close(); },
    };
}

test('bounded field codec rejects truncation, duplicate/trailing data', () => {
    const bytes = fieldsEncode({ RoomCode: 'ABCDEFGH', Protocol: '1' });
    assert.equal(fieldsDecode(bytes).RoomCode, 'ABCDEFGH');
    for (let length = 0; length < bytes.length; ++length) assert.throws(() => fieldsDecode(bytes.subarray(0, length)));
    assert.throws(() => fieldsDecode(Buffer.concat([bytes, Buffer.from([0])])));
    const one = fieldsEncode({ RoomCode: 'ABCDEFGH' });
    const duplicate = Buffer.concat([one, one.subarray(34)]); duplicate[33] = 2;
    assert.throws(() => fieldsDecode(duplicate));
    const window = new ReplayWindow();
    for (const seq of [3n, 1n, 2n, 70n, 69n]) assert.equal(window.accept(seq), true);
    for (const seq of [0n, 70n, 2n, 69n]) assert.equal(window.accept(seq), false);
});

test('real HTTP/UDP: room lifecycle, auth, channels, replay, rebinding and expiry', async t => {
    let now = 1000;
    const service = createService({ httpPort: 0, udpPort: 0, backendUrl: 'https://backend.example', now: () => now, leaseMs: 30000 });
    const address = await service.listen(); t.after(() => service.close());
    const host = await peer(), guest = await peer(), other = await peer();
    t.after(() => { host.close(); guest.close(); other.close(); });
    async function post(path, values) {
        const result = await fetch(`http://127.0.0.1:${address.httpPort}/v1/rooms/${path}`, {
            method: 'POST', body: fieldsEncode({ Protocol: '1', Build: '3.290.1.0', ...values }),
        });
        return { status: result.status, fields: fieldsDecode(Buffer.from(await result.arrayBuffer())) };
    }
    const created = await post('create', { Store: 'epic' });
    assert.equal(created.status, 201);
    assert.match(created.fields.RoomCode, /^\d{5}$/);
    assert.equal(created.fields.BackendUrl, 'https://backend.example');
    const h = created.fields;
    let hs = 0n, gs = 0n;
    const pack = (c, role, kind, channel, seq, payload) => packetEncode(c.RoomId, Buffer.from(c.Token, 'hex'), role, kind, channel, seq, payload);
    const unpack = (bytes, c) => bytes && packetDecode(bytes, Buffer.from(c.Token, 'hex'));
    assert.equal((await post('join', { Store: 'steam', RoomCode: h.RoomCode })).status, 404);
    host.send(pack(h, 0, Kind.hello, 0, ++hs), address.udpPort);
    assert.equal(unpack(await host.receive(), h).kind, Kind.ack);
    const joined = await post('join', { Store: 'steam', RoomCode: ` ${h.RoomCode.slice(0, 2)}-${h.RoomCode.slice(2)}` });
    assert.equal(joined.status, 200); const g = joined.fields;
    assert.equal((await post('join', { Store: 'steam', RoomCode: h.RoomCode })).status, 409);
    guest.send(pack(g, 1, Kind.hello, 0, ++gs), address.udpPort);
    assert.equal(unpack(await guest.receive(), g).kind, Kind.ack);
    for (const channel of [1, 2, 3]) {
        const payload = Buffer.from([0x05, 0xff, 0, 0xad, channel]);
        const message = pack(g, 1, Kind.up, channel, ++gs, payload);
        guest.send(message, address.udpPort);
        const received = unpack(await host.receive(), h);
        assert.equal(received.kind, Kind.down); assert.equal(received.channel, channel); assert.deepEqual(received.payload, payload);
        guest.send(message, address.udpPort);
        assert.equal(await host.receive(40), null, 'replay must not forward');
        host.send(pack(h, 0, Kind.up, channel, ++hs, payload), address.udpPort);
        assert.deepEqual(unpack(await guest.receive(), g).payload, payload);
    }
    const tampered = pack(g, 1, Kind.up, 1, ++gs, Buffer.from('original')); tampered[32] ^= 1;
    guest.send(tampered, address.udpPort);
    assert.equal(await host.receive(40), null, 'modified payload must not forward');
    guest.send(pack(g, 1, Kind.down, 1, ++gs, Buffer.from('reflection')), address.udpPort);
    assert.equal(await host.receive(40), null, 'wrong direction must not forward');
    // Authenticated newer packet from a new source rebinds the NAT mapping.
    other.send(pack(g, 1, Kind.hello, 0, ++gs), address.udpPort);
    assert.equal(unpack(await other.receive(), g).kind, Kind.ack);
    host.send(pack(h, 0, Kind.up, 2, ++hs, Buffer.from('rebound')), address.udpPort);
    assert.equal(unpack(await other.receive(), g).payload.toString(), 'rebound');
    assert.equal(await guest.receive(40), null);
    assert.equal((await post('leave', g)).status, 200);
    assert.equal(unpack(await other.receive(), g).kind, Kind.gone);
    other.send(pack(g, 1, Kind.up, 1, ++gs, Buffer.from('stale guest')), address.udpPort);
    assert.equal(await host.receive(40), null, 'retired guest token must not forward');
    const again = await post('join', { Store: 'steam', RoomCode: h.RoomCode });
    assert.equal(again.status, 200); assert.notEqual(again.fields.Token, g.Token);
    now += 31000; service.sweep();
    assert.equal(unpack(await host.receive(), h).kind, Kind.gone);
    assert.equal((await post('join', { Store: 'steam', RoomCode: h.RoomCode })).status, 404);
    const reverse = await post('create', { Store: 'steam' });
    assert.equal(reverse.status, 201);
    const rh = reverse.fields;
    host.send(pack(rh, 0, Kind.hello, 0, ++hs), address.udpPort);
    assert.equal(unpack(await host.receive(), rh).kind, Kind.ack);
    assert.equal((await post('join', { Store: 'epic', RoomCode: rh.RoomCode })).status, 200);
    assert.equal((await post('create', { Store: 'epic', Protocol: '99' })).status, 409);
});
