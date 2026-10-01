// The relay's rooms and its WebSocket server, through Node's own WebSocket
// client: node --test relay/

import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { connect as tcpConnect } from 'node:net';
import { after, before, test } from 'node:test';

import { cloudflareTurn, createRelay, DEFAULT_ICE } from './relay.mjs';

let relay, url;
before(async () => {
    relay = createRelay({ limits: { loneSeconds: 0.5, messageBurst: 20, messagesPerSecond: 1, probeSeconds: 0.3 } });
    await new Promise(resolve => relay.listen(0, '127.0.0.1', resolve));
    url = `ws://127.0.0.1:${relay.address().port}`;
});
after(() => relay.close());

// A connection, with its messages in order and how it closed.
async function connect() {
    const ws = new WebSocket(url);
    const queue = [], waiting = [];
    ws.onmessage = event => {
        const message = JSON.parse(event.data);
        if (waiting.length) waiting.shift()(message);
        else queue.push(message);
    };
    const closed = new Promise(resolve => { ws.onclose = event => resolve(event.code); });
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    const next = () => queue.length ? Promise.resolve(queue.shift()) : new Promise(resolve => waiting.push(resolve));
    const welcome = await next();
    return { ws, next, closed, welcome, send: message => ws.send(JSON.stringify(message)) };
}

test('says hello with the ICE servers to use', async () => {
    const a = await connect();
    assert.equal(a.welcome.relay, 1);
    assert.ok(Array.isArray(a.welcome.ice) && a.welcome.ice.length > 0);
    a.ws.close();
});

test('a code opens one room at a time', async () => {
    const host = await connect();
    host.send({ host: 'K7QF2M' });
    assert.deepEqual(await host.next(), { hosting: 'K7QF2M' });
    const other = await connect();
    other.send({ host: 'K7QF2M' });
    assert.deepEqual(await other.next(), { taken: 'K7QF2M' });
    other.send({ host: 'K7QF2N' });
    assert.deepEqual(await other.next(), { hosting: 'K7QF2N' });
    host.ws.close();
    other.ws.close();
});

test('joining a code no room has', async () => {
    const joiner = await connect();
    joiner.send({ join: 'ZZZZZZ' });
    assert.deepEqual(await joiner.next(), { missing: 'ZZZZZZ' });
    joiner.ws.close();
});

test('signals go between the host and each joiner', async () => {
    const host = await connect();
    host.send({ host: 'ABCDEF' });
    await host.next();
    const a = await connect();
    const b = await connect();
    a.send({ join: 'ABCDEF' });
    assert.deepEqual(await a.next(), { joined: 'ABCDEF' });
    assert.deepEqual(await host.next(), { peer: 1 });
    b.send({ join: 'ABCDEF' });
    assert.deepEqual(await b.next(), { joined: 'ABCDEF' });
    assert.deepEqual(await host.next(), { peer: 2 });

    a.send({ signal: { type: 'offer', sdp: 'a' } });
    assert.deepEqual(await host.next(), { from: 1, signal: { type: 'offer', sdp: 'a' } });
    host.send({ to: 2, signal: { candidate: 'x' } });
    assert.deepEqual(await b.next(), { signal: { candidate: 'x' } });
    host.send({ to: 1, signal: { type: 'answer', sdp: 'b' } });
    assert.deepEqual(await a.next(), { signal: { type: 'answer', sdp: 'b' } });

    // A joiner hanging up; then the host, which closes the room
    a.ws.close();
    assert.deepEqual(await host.next(), { left: 1 });
    host.ws.close();
    assert.deepEqual(await b.next(), { closed: 'ABCDEF' });
    assert.equal(await b.closed, 1000);
    assert.equal(relay.rooms.has('ABCDEF'), false);
});

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// A host that never answers pings, like one whose machine went away without
// its connection closing: a WebSocket by hand, which only talks.
async function deafHost() {
    const socket = tcpConnect(relay.address().port, '127.0.0.1');
    await new Promise(resolve => socket.once('connect', resolve));
    socket.write(`GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n`
        + `Sec-WebSocket-Key: ${randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
    let buffer = Buffer.alloc(0);
    let upgraded = false;
    const queue = [], waiting = [];
    socket.on('data', chunk => {
        buffer = Buffer.concat([buffer, chunk]);
        if (!upgraded) {
            const end = buffer.indexOf('\r\n\r\n');
            if (end < 0) return;
            buffer = buffer.subarray(end + 4);
            upgraded = true;
        }
        while (buffer.length >= 2) {
            const opcode = buffer[0] & 0x0f;
            let size = buffer[1] & 0x7f, at = 2;
            if (size === 126) {
                if (buffer.length < 4) return;
                size = buffer.readUInt16BE(2);
                at = 4;
            }
            if (buffer.length < at + size) return;
            const payload = buffer.subarray(at, at + size);
            buffer = buffer.subarray(at + size);
            if (opcode !== 0x1) continue; // Pings go unanswered
            const message = JSON.parse(payload.toString());
            if (waiting.length) waiting.shift()(message);
            else queue.push(message);
        }
    });
    const next = () => queue.length ? Promise.resolve(queue.shift()) : new Promise(resolve => waiting.push(resolve));
    const send = message => {
        const payload = Buffer.from(JSON.stringify(message));
        const mask = randomBytes(4);
        socket.write(Buffer.concat([Buffer.from([0x81, 0x80 | payload.length]), mask,
            Buffer.from(payload.map((b, i) => b ^ mask[i & 3]))]));
    };
    await next(); // The hello
    return { socket, next, send };
}

test('players take a room over once its host is gone', async () => {
    const host = await connect();
    host.send({ host: 'MGRATE', key: 'k1' });
    assert.deepEqual(await host.next(), { hosting: 'MGRATE' });
    host.ws.close();
    while (relay.rooms.has('MGRATE')) await sleep(10);
    const a = await connect();
    a.send({ migrate: 'MGRATE', key: 'k1' });
    assert.deepEqual(await a.next(), { hosting: 'MGRATE' });
    const b = await connect();
    b.send({ migrate: 'MGRATE', key: 'k1' }); // Just taken over: b joins a
    assert.deepEqual(await b.next(), { joined: 'MGRATE' });
    assert.deepEqual(await a.next(), { peer: 1 });
    a.ws.close();
    b.ws.close();
});

test('a host that answers keeps its room, and its players join it', async () => {
    const host = await connect(); // Node's WebSocket answers pings by itself
    host.send({ host: 'KEEPXT', key: 'k2' });
    assert.deepEqual(await host.next(), { hosting: 'KEEPXT' });
    await sleep(350); // Long enough for the relay to ask
    const a = await connect();
    a.send({ migrate: 'KEEPXT', key: 'k2' });
    assert.deepEqual(await a.next(), { joined: 'KEEPXT' });
    assert.deepEqual(await host.next(), { peer: 1 });
    host.ws.close();
    a.ws.close();
});

test('a host that doesn\'t answer loses its room to its players', async () => {
    const host = await deafHost();
    host.send({ host: 'DEAFHS', key: 'k3' });
    assert.deepEqual(await host.next(), { hosting: 'DEAFHS' });
    await sleep(350);
    const a = await connect();
    const b = await connect();
    a.send({ migrate: 'DEAFHS', key: 'k3' });
    await sleep(50);
    b.send({ migrate: 'DEAFHS', key: 'k3' }); // While the relay waits on the host
    assert.deepEqual(await a.next(), { hosting: 'DEAFHS' });
    assert.deepEqual(await b.next(), { joined: 'DEAFHS' });
    assert.deepEqual(await a.next(), { peer: 1 });
    assert.deepEqual(await host.next(), { lost: 'DEAFHS' });
    host.socket.destroy();
    a.ws.close();
    b.ws.close();
});

test('another key is another match', async () => {
    const host = await connect();
    host.send({ host: 'KEYED2', key: 'right' });
    assert.deepEqual(await host.next(), { hosting: 'KEYED2' });
    const a = await connect();
    a.send({ migrate: 'KEYED2', key: 'wrong' });
    assert.deepEqual(await a.next(), { taken: 'KEYED2' });
    const b = await connect();
    b.send({ migrate: 'KEYED2', key: 'not a key!' });
    assert.equal(await b.closed, 1008);
    host.ws.close();
    a.ws.close();
});

test('a code has 6 characters without look-alikes', async () => {
    for (const code of ['K7QF2', 'K7QF2MM', 'K7QF2O', 'k7qf2m', 'K7QF21']) {
        const a = await connect();
        a.send({ host: code });
        assert.equal(await a.closed, 1008, code);
    }
});

test('connections that never join a room are closed', async () => {
    const a = await connect();
    assert.equal(await a.closed, 1008);
});

test('too many messages close the connection', async () => {
    const host = await connect();
    host.send({ host: 'RATE22' });
    await host.next();
    for (let i = 0; i < 40; i++) host.send({ to: 99, signal: i });
    assert.equal(await host.closed, 1008);
    assert.equal(relay.rooms.has('RATE22'), false);
});

test('messages past the size limit close the connection', async () => {
    const host = await connect();
    host.send({ host: 'BXG222' });
    await host.next();
    host.send({ to: 1, signal: 'x'.repeat(70000) });
    assert.equal(await host.closed, 1002);
});

test('answers plain HTTP, for health checks', async () => {
    const response = await fetch(url.replace('ws:', 'http:'));
    assert.equal(response.status, 200);
    assert.match(await response.text(), /Tide relay/);
});

// Cloudflare's answer, as its docs show it, port 53 and all.
const CLOUDFLARE = {
    iceServers: [
        { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
        {
            urls: ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:53?transport=udp',
                'turns:turn.cloudflare.com:443?transport=tcp'],
            username: 'user',
            credential: 'secret',
        },
    ],
};

test('TURN credentials from Cloudflare, made once and shared', async () => {
    const calls = [];
    const fetch = async (url, init) => {
        calls.push({ url, init });
        return { ok: true, status: 201, json: async () => CLOUDFLARE };
    };
    const ice = cloudflareTurn('KEY', 'TOKEN', { fetch });
    const turnRelay = createRelay({ iceServers: ice });
    await new Promise(resolve => turnRelay.listen(0, '127.0.0.1', resolve));
    const at = `ws://127.0.0.1:${turnRelay.address().port}`;
    const hello = async () => {
        const ws = new WebSocket(at);
        const message = await new Promise(resolve => { ws.onmessage = event => resolve(JSON.parse(event.data)); });
        ws.close();
        return message;
    };
    const [a, b] = await Promise.all([hello(), hello()]);
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'https://rtc.live.cloudflare.com/v1/turn/keys/KEY/credentials/generate-ice-servers');
    assert.equal(calls[0].init.headers.authorization, 'Bearer TOKEN');
    assert.deepEqual(JSON.parse(calls[0].init.body), { ttl: 172800 });
    assert.deepEqual(a, b);
    assert.deepEqual(a.ice, [
        { urls: ['stun:stun.cloudflare.com:3478'] },
        { urls: ['turn:turn.cloudflare.com:3478?transport=udp', 'turns:turn.cloudflare.com:443?transport=tcp'],
            username: 'user', credential: 'secret' },
    ]);
    turnRelay.close();
});

test('without TURN credentials, players get STUN only', async () => {
    const fetch = async () => ({ ok: false, status: 401, json: async () => ({}) });
    const errors = console.error;
    console.error = () => {};
    const ice = await cloudflareTurn('KEY', 'WRONG', { fetch })();
    console.error = errors;
    assert.deepEqual(ice, DEFAULT_ICE);
});
