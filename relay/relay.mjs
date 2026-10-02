// The relay: how players find each other's matches by a room code (see
// AGENTS.md, Networking). A machine hosting a match opens a room under a code
// it picked, and players joining with that code are introduced to it: the
// relay passes along what WebRTC needs to connect them (offers, answers and
// ICE candidates). Their packets then go straight between them, never through
// here. Only the introductions do, so a room costs almost nothing.
//
// It speaks JSON over WebSocket, with a small WebSocket server of its own, so
// it needs nothing but Node. Messages, one object each:
//
//   relay -> anyone   {relay: 1, ice: [...]}     On connecting: the ICE servers to use
//   host  -> relay    {host: "K7QF2M", key: K}   Opens a room; the key is optional
//   relay -> host     {hosting: "K7QF2M"}        ...it's open for as long as this connection is
//                     {taken: "K7QF2M"}          ...or the code is in use: pick another
//   joiner -> relay   {join: "K7QF2M"}
//   relay -> joiner   {joined: "K7QF2M"}         Signals now go to the room's host
//                     {missing: "K7QF2M"}        No room has that code
//                     {full: "K7QF2M"}           Too many players are joining it at once
//   relay -> host     {peer: 3}                  Player 3 is joining
//                     {left: 3}                  ...or gave up, or connected and hung up: a player hangs
//                                                up once the host sent it something over their channel,
//                                                so a host that hasn't seen its channel open yet can
//                                                take {left} for giving up
//   joiner -> relay   {signal: ...}              Passed on to the host as {from: 3, signal: ...}
//   host  -> relay    {to: 3, signal: ...}       Passed on to player 3 as {signal: ...}
//   relay -> joiner   {closed: "K7QF2M"}         The host closed the room
//
// Host migration: when a room's host goes, the players of its match come back
// with the key it opened the room with, which only they know.
//
//   player -> relay   {migrate: "K7QF2M", key: K}
//   relay -> player   {hosting: "K7QF2M"}        The host is gone: this player hosts the room now
//                     {joined: "K7QF2M"}         ...or another player got there first, or the host
//                                                answered a ping after all: introduced to it
//                     {taken: "K7QF2M"}          The room has another key: not the same match
//   relay -> host     {lost: "K7QF2M"}           The host didn't answer: the room is someone else's
//   host  -> relay    {end: "K7QF2M"}            Its match ended for good: it closes the room, and
//                                                for a while the relay tells its players who come
//                                                back {ended: "K7QF2M"} rather than let them take it
//                                                over, whatever goodbye of the host's they missed.
//                                                A host whose match ended before the relay knew the
//                                                room (on this connection) sends {host} first, at
//                                                once: messages are read in order, hello or not
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

import { createHash } from 'node:crypto';
import { createServer } from 'node:http';

// Codes are 6 characters without look-alikes (no 0, O, 1 or I): about a billion.
export const CODE_LETTERS = '23456789ABCDEFGHJKLMNPQRSTUVWXYZ';
const CODE = /^[2-9A-HJ-NP-Z]{6}$/;
const KEY = /^[0-9A-Za-z]{1,32}$/;

const LIMITS = {
    message: 64 * 1024,     // Bytes; offers are a few kilobytes
    joining: 32,            // Players joining one room at the same time
    perAddress: 64,         // Connections from one address
    connections: 20000,     // In all
    messagesPerSecond: 50,  // Each connection's, on average...
    messageBurst: 200,      // ...with this many at once
    loneSeconds: 30,        // A connection that isn't in a room by then is closed
    pingSeconds: 25,        // Keeps connections through proxies, and finds dead ones
    probeSeconds: 3,        // How long a host has to answer a ping when its players come to take its room over
    endedSeconds: 300,      // How long a room whose match ended stays so, for its players who missed it
};

// STUN only: players find their own addresses, and connect directly or not at all.
export const DEFAULT_ICE = [{ urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.l.google.com:19302'] }];

// Cloudflare's TURN, for players whose networks can't connect directly: its
// servers carry their packets. Its key (fly.toml says where it's kept) makes
// credentials, which every player gets with the relay's hello. They're made
// once an hour and last two days, so a match that started with them keeps its
// relayed connection. Without them, it's STUN only. `options.fetch` is for tests.
export function cloudflareTurn(keyId, token, options = {}) {
    const fetch = options.fetch || globalThis.fetch;
    const url = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(keyId)}/credentials/generate-ice-servers`;
    let servers = null;
    let made = 0;
    let making = null;
    async function make() {
        try {
            const response = await fetch(url, {
                method: 'POST',
                headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json' },
                body: JSON.stringify({ ttl: 2 * 24 * 3600 }),
            });
            if (!response.ok) throw new Error(`Cloudflare answered ${response.status}`);
            const body = await response.json();
            const list = Array.isArray(body.iceServers) ? body.iceServers : [body.iceServers];
            // Browsers block port 53, and a TURN URL on it only times out (Cloudflare's docs)
            servers = list.filter(s => s && s.urls).map(s => ({
                ...s,
                urls: [].concat(s.urls).filter(u => !/:53(\?|$)/.test(u)),
            }));
            made = Date.now();
        } catch (error) {
            console.error(`relay: no TURN credentials: ${error.message}`);
        }
    }
    return async () => {
        if (!servers || Date.now() - made > 3600 * 1000) {
            making = making || make().finally(() => { making = null; });
            await making;
        }
        return servers || DEFAULT_ICE;
    };
}

// A relay on an HTTP server of its own, which answers anything that isn't a
// WebSocket with a line of text (for health checks). Call listen() on it.
// `iceServers`: what players are told to use, or a function that gives it (a
// promise will do). `trustProxy`: take the client's address from Fly's
// Fly-Client-IP header.
export function createRelay({ iceServers = DEFAULT_ICE, trustProxy = false, limits = {} } = {}) {
    const limit = { ...LIMITS, ...limits };
    const rooms = new Map(); // Code -> room
    const ended = new Map(); // Code -> {key, until}: rooms whose match ended, which no one takes over
    const perAddress = new Map();
    const sockets = new Set();
    let connections = 0;

    const server = createServer((request, response) => {
        response.writeHead(200, { 'content-type': 'text/plain' });
        response.end(`Tide relay: ${rooms.size} room${rooms.size === 1 ? '' : 's'}\n`);
    });

    server.on('upgrade', (request, socket) => {
        const address = (trustProxy && request.headers['fly-client-ip']) || socket.remoteAddress || '?';
        const count = perAddress.get(address) || 0;
        if (connections >= limit.connections || count >= limit.perAddress) {
            socket.end('HTTP/1.1 503 Service Unavailable\r\n\r\n');
            return;
        }
        const ws = accept(request, socket, limit.message, sockets);
        if (!ws) return;
        connections++;
        perAddress.set(address, count + 1);
        const peer = { ws, room: null, id: 0, claiming: null, tokens: limit.messageBurst, last: Date.now() };
        const lone = setTimeout(() => { if (!peer.room) ws.close(1008, 'not in a room'); }, limit.loneSeconds * 1000);
        ws.onmessage = text => receive(peer, text);
        ws.onclose = () => {
            clearTimeout(lone);
            connections--;
            const left = perAddress.get(address) - 1;
            if (left > 0) perAddress.set(address, left);
            else perAddress.delete(address);
            leave(peer);
        };
        Promise.resolve(typeof iceServers === 'function' ? iceServers() : iceServers)
            .then(ice => ws.send({ relay: 1, ice }), () => ws.send({ relay: 1, ice: DEFAULT_ICE }));
    });

    function receive(peer, text) {
        // Messages refill at a steady rate, up to a burst
        const now = Date.now();
        peer.tokens = Math.min(limit.messageBurst, peer.tokens + (now - peer.last) * limit.messagesPerSecond / 1000);
        peer.last = now;
        if (--peer.tokens < 0) {
            peer.ws.close(1008, 'too many messages');
            return;
        }
        let message;
        try {
            message = JSON.parse(text);
        } catch {
            peer.ws.close(1007, 'not JSON');
            return;
        }
        if (!message || typeof message !== 'object') return;
        const room = peer.room;

        if (typeof message.host === 'string' && !room && !peer.claiming) {
            const code = message.host;
            const key = typeof message.key === 'string' && KEY.test(message.key) ? message.key : null;
            if (!CODE.test(code)) peer.ws.close(1008, 'not a room code');
            else if (rooms.has(code) || endedRoom(code)) peer.ws.send({ taken: code });
            else host(peer, code, key);
        } else if (typeof message.end === 'string' && room && room.host === peer && message.end === room.code) {
            if (room.key) ended.set(room.code, { key: room.key, until: Date.now() + limit.endedSeconds * 1000 });
            leave(peer);
        } else if (typeof message.join === 'string' && !room && !peer.claiming) {
            const code = message.join;
            const joined = rooms.get(code);
            if (!CODE.test(code)) peer.ws.close(1008, 'not a room code');
            else if (!joined) peer.ws.send({ missing: code });
            else join(peer, joined);
        } else if (typeof message.migrate === 'string' && !room && !peer.claiming) {
            const code = message.migrate;
            if (!CODE.test(code) || typeof message.key !== 'string' || !KEY.test(message.key)) {
                peer.ws.close(1008, 'not a room code and key');
            } else {
                migrate(peer, code, message.key);
            }
        } else if ('signal' in message && room) {
            if (room.host === peer) {
                const to = room.joining.get(message.to);
                if (to) to.ws.send({ signal: message.signal });
            } else {
                room.host.ws.send({ from: peer.id, signal: message.signal });
            }
        }
    }

    function host(peer, code, key) {
        peer.room = { code, host: peer, joining: new Map(), next: 1, key, since: Date.now(), claims: null };
        rooms.set(code, peer.room);
        peer.ws.send({ hosting: code });
    }

    function join(peer, room) {
        if (room.joining.size >= limit.joining) {
            peer.ws.send({ full: room.code });
            return;
        }
        peer.room = room;
        peer.id = room.next++;
        room.joining.set(peer.id, peer);
        peer.ws.send({ joined: room.code });
        room.host.ws.send({ peer: peer.id });
    }

    // Host migration: a player of the match in room `code` comes back to it,
    // with its key. If the room is gone, the player hosts it. If its host is
    // there, the player joins: one that answers a ping in time, or one who
    // took the room over a moment ago. Otherwise the host is dropped, and the
    // first of the players asking hosts the room, which the others join.
    function migrate(peer, code, key) {
        const room = rooms.get(code);
        const gone = endedRoom(code);
        if (gone && gone.key === key) {
            peer.ws.send({ ended: code });
        } else if (gone) {
            peer.ws.send({ taken: code });
        } else if (!room) {
            host(peer, code, key);
        } else if (room.key !== key) {
            peer.ws.send({ taken: code });
        } else if (room.claims) { // Its host was pinged already
            room.claims.push(peer);
            peer.claiming = room;
        } else if (Date.now() - room.since < limit.probeSeconds * 1000) {
            join(peer, room);
        } else {
            room.claims = [peer];
            peer.claiming = room;
            room.host.ws.probe(limit.probeSeconds * 1000, alive => settle(room, alive));
        }
    }

    function settle(room, alive) {
        const claims = room.claims.filter(p => p.claiming === room);
        room.claims = null;
        for (const p of claims) p.claiming = null;
        if (alive && rooms.get(room.code) === room) {
            for (const p of claims) join(p, room);
            return;
        }
        if (rooms.get(room.code) === room) { // The host didn't answer: the room goes to its players
            const old = room.host;
            rooms.delete(room.code);
            old.room = null;
            old.ws.send({ lost: room.code });
            closeJoiners(room);
        }
        for (const p of claims) migrate(p, room.code, room.key); // The first hosts it again, the others join
    }

    // A room whose match ended a moment ago, or null
    function endedRoom(code) {
        const gone = ended.get(code);
        if (gone && gone.until > Date.now()) return gone;
        ended.delete(code);
        return null;
    }

    function closeJoiners(room) {
        for (const joiner of room.joining.values()) {
            joiner.room = null;
            joiner.ws.send({ closed: room.code });
            joiner.ws.close(1000, 'the room closed');
        }
    }

    function leave(peer) {
        if (peer.claiming) {
            peer.claiming.claims = peer.claiming.claims.filter(p => p !== peer);
            peer.claiming = null;
        }
        const room = peer.room;
        if (!room) return;
        peer.room = null;
        if (room.host === peer) {
            rooms.delete(room.code);
            closeJoiners(room);
        } else {
            room.joining.delete(peer.id);
            room.host.ws.send({ left: peer.id });
        }
    }

    const pings = setInterval(() => {
        for (const ws of sockets) ws.ping();
        for (const code of [...ended.keys()]) endedRoom(code); // Forgets those whose time is up
    }, limit.pingSeconds * 1000);
    pings.unref();
    server.on('close', () => clearInterval(pings));
    server.rooms = rooms;
    server.ended = ended;
    return server;
}

// ---------------------------------------------------------------------------
// WebSocket (RFC 6455), as much as browsers use: text messages, pings and
// closing. Frames from clients are masked; ours aren't. `sockets` holds the
// open ones.

function accept(request, socket, maxMessage, sockets) {
    const key = request.headers['sec-websocket-key'];
    if (!/websocket/i.test(request.headers.upgrade || '') || !key || request.headers['sec-websocket-version'] !== '13') {
        socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
        return null;
    }
    const acceptKey = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
        + `Sec-WebSocket-Accept: ${acceptKey}\r\n\r\n`);
    socket.setNoDelay(true);

    let buffer = Buffer.alloc(0);
    let parts = [];      // A message in fragments, so far
    let partsSize = 0;
    let closed = false;
    let answered = true; // The last ping got its pong
    const probes = new Set(); // What waits on the next pong
    const ws = {
        onmessage: () => {},
        onclose: () => {},
        send(message) {
            if (!closed) socket.write(frame(0x1, Buffer.from(JSON.stringify(message))));
        },
        ping() {
            if (!answered) {
                end();
                return;
            }
            answered = false;
            if (!closed) socket.write(frame(0x9, Buffer.alloc(0)));
        },
        // Pings now: done(true) when a pong comes within `ms`, done(false) if not
        probe(ms, done) {
            if (closed) {
                done(false);
                return;
            }
            let settled = false;
            const settle = alive => {
                if (settled) return;
                settled = true;
                probes.delete(settle);
                done(alive);
            };
            probes.add(settle);
            socket.write(frame(0x9, Buffer.alloc(0)));
            setTimeout(() => settle(false), ms).unref();
        },
        close(code = 1000, reason = '') {
            if (closed) return;
            const body = Buffer.alloc(2 + Buffer.byteLength(reason));
            body.writeUInt16BE(code, 0);
            body.write(reason, 2);
            socket.write(frame(0x8, body));
            end();
        },
    };

    function end() {
        if (closed) return;
        closed = true;
        sockets.delete(ws);
        socket.end();
        for (const settle of [...probes]) settle(false);
        ws.onclose();
    }

    socket.on('data', chunk => {
        buffer = Buffer.concat([buffer, chunk]);
        for (;;) {
            if (buffer.length < 2) return;
            const fin = (buffer[0] & 0x80) !== 0;
            const opcode = buffer[0] & 0x0f;
            const masked = (buffer[1] & 0x80) !== 0;
            let size = buffer[1] & 0x7f;
            let at = 2;
            if (size === 126) {
                if (buffer.length < 4) return;
                size = buffer.readUInt16BE(2);
                at = 4;
            } else if (size === 127) {
                if (buffer.length < 10) return;
                const big = buffer.readBigUInt64BE(2);
                size = big > BigInt(maxMessage) ? maxMessage + 1 : Number(big);
                at = 10;
            }
            if (!masked || size > maxMessage || (buffer[0] & 0x70) !== 0) {
                ws.close(1002, 'protocol error');
                return;
            }
            if (buffer.length < at + 4 + size) return;
            const mask = buffer.subarray(at, at + 4);
            const payload = Buffer.from(buffer.subarray(at + 4, at + 4 + size));
            for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
            buffer = buffer.subarray(at + 4 + size);

            if (opcode === 0x8) {
                ws.close(1000);
                return;
            } else if (opcode === 0x9) {
                if (!closed) socket.write(frame(0xa, payload));
            } else if (opcode === 0xa) {
                answered = true;
                for (const settle of [...probes]) settle(true);
            } else if (opcode === 0x1 || opcode === 0x0) {
                if ((opcode === 0x1) !== (parts.length === 0) || partsSize + size > maxMessage) {
                    ws.close(1002, 'protocol error');
                    return;
                }
                parts.push(payload);
                partsSize += size;
                if (fin) {
                    const text = Buffer.concat(parts).toString('utf8');
                    parts = [];
                    partsSize = 0;
                    ws.onmessage(text);
                    if (closed) return;
                }
            } else {
                ws.close(1003, 'only text');
                return;
            }
        }
    });
    socket.on('close', end);
    socket.on('error', end);
    sockets.add(ws);
    return ws;
}

function frame(opcode, payload) {
    const size = payload.length;
    const head = size < 126 ? Buffer.from([0x80 | opcode, size])
        : size < 65536 ? Buffer.from([0x80 | opcode, 126, size >> 8, size & 0xff])
        : Buffer.concat([Buffer.from([0x80 | opcode, 127]), bigEndian64(size)]);
    return Buffer.concat([head, payload]);
}

function bigEndian64(n) {
    const b = Buffer.alloc(8);
    b.writeBigUInt64BE(BigInt(n));
    return b;
}
