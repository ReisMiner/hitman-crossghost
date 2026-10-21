import { createHmac, timingSafeEqual } from 'node:crypto';

// GhostCrossplay's bounded lobby_metadata.h field encoding. HTTP envelopes
// use response=true (type 2), nonce=1, generation=1; no lobby metadata is stored here.
const magic = Buffer.from([...'GHOST-LOBBY-'].map(c => c.charCodeAt(0)).concat([0x91, 0x27, 1, 0xd3]));
export function fieldsEncode(fields) {
    const entries = Object.entries(fields);
    if (entries.length > 24) throw new Error('too many fields');
    const header = Buffer.alloc(34);
    magic.copy(header); header[16] = 2; header.writeBigUInt64LE(1n, 17); header.writeBigUInt64LE(1n, 25); header[33] = entries.length;
    const parts = [header];
    for (const [key, value] of entries) {
        const k = Buffer.from(key), v = Buffer.from(String(value));
        if (!k.length || k.length > 96 || v.length > 512 || k.includes(0) || v.includes(0)) throw new Error('invalid field');
        const sizes = Buffer.alloc(3); sizes[0] = k.length; sizes.writeUInt16LE(v.length, 1);
        parts.push(sizes, k, v);
    }
    const packet = Buffer.concat(parts);
    if (packet.length > 4096) throw new Error('envelope too large');
    return packet;
}
export function fieldsDecode(packet) {
    if (packet.length < 34 || packet.length > 4096 || !packet.subarray(0, 16).equals(magic) ||
        packet[16] !== 2 || packet.readBigUInt64LE(17) !== 1n || packet.readBigUInt64LE(25) !== 1n || packet[33] > 24)
        throw new Error('invalid envelope');
    const fields = Object.create(null);
    let at = 34;
    for (let i = 0; i < packet[33]; i++) {
        if (at + 3 > packet.length) throw new Error('truncated field');
        const nk = packet[at], nv = packet.readUInt16LE(at + 1); at += 3;
        if (!nk || nk > 96 || nv > 512 || at + nk + nv > packet.length) throw new Error('invalid field length');
        const key = packet.toString('utf8', at, at + nk); at += nk;
        const value = packet.toString('utf8', at, at + nv); at += nv;
        if (Object.hasOwn(fields, key) || key.includes('\0') || value.includes('\0')) throw new Error('invalid field');
        fields[key] = value;
    }
    if (at !== packet.length) throw new Error('trailing bytes');
    return fields;
}

// 32-byte header + payload + HMAC-SHA256. Authenticated direction prevents a
// client packet being reflected back as a server response. Tokens never use UDP.
export const MAX_PAYLOAD = 65443;
export const Kind = Object.freeze({ hello: 1, ack: 2, up: 3, down: 4, gone: 5 });
export function packetEncode(id, token, role, kind, channel, sequence, payload = Buffer.alloc(0)) {
    if (payload.length > MAX_PAYLOAD) throw new Error('payload too large');
    const head = Buffer.alloc(32);
    head.write('GCR1'); head[4] = kind; head[5] = role; head[6] = channel;
    Buffer.from(id, 'hex').copy(head, 8); head.writeBigUInt64LE(sequence, 24);
    const data = Buffer.concat([head, payload]);
    return Buffer.concat([data, createHmac('sha256', token).update(data).digest()]);
}
export function packetDecode(packet, token) {
    if (packet.length < 64 || packet.length > 65507 || packet.toString('ascii', 0, 4) !== 'GCR1' ||
        packet[7] !== 0 || packet[5] > 1) return null;
    const data = packet.subarray(0, -32);
    if (!timingSafeEqual(createHmac('sha256', token).update(data).digest(), packet.subarray(-32))) return null;
    return { id: packet.toString('hex', 8, 24), role: packet[5], kind: packet[4], channel: packet[6],
        sequence: packet.readBigUInt64LE(24), payload: packet.subarray(32, -32) };
}
export class ReplayWindow {
    high = 0n;
    bits = 0n;
    accept(sequence) {
        if (!sequence) return false;
        if (sequence > this.high) {
            const shift = sequence - this.high;
            this.bits = shift >= 64n ? 1n : ((this.bits << shift) | 1n) & 0xffffffffffffffffn;
            this.high = sequence;
            return true;
        }
        const delta = this.high - sequence;
        if (delta >= 64n || (this.bits & (1n << delta))) return false;
        this.bits |= 1n << delta;
        return true;
    }
}
