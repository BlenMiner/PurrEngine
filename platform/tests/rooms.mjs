// Rooms on desktop, through a relay on this machine (relay/relay.mjs):
//
// - two desktop players in a match (rooms_native.c's host and join);
// - a desktop host, and a browser joining its room;
// - a browser hosting, and a desktop player joining its room.
//
// The browsers are headless Chrome (or Edge) and Firefox, whichever are
// here, on a page of their own that speaks the relay's protocol as tide.js
// does and echoes every message on its data channel. So the desktop's WebRTC
// (platform/src/rtc) is checked against each browser's, both ways round.
// Without a browser, only the first runs.
//
//   node rooms.mjs <Chrome or Edge> <tide_platform_rooms> <scratch folder> [Firefox]
//
// With TIDE_RELAY set, like wss://relay.tide-engine.dev, everyone meets
// through that relay instead, to check a deployed one. TIDE_ICE_POLICY=relay
// sends every packet through its TURN servers, both the browser's and ours.

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { resolve } from 'node:path';

import { createRelay } from '../../relay/relay.mjs';

const [browser, program, scratch, firefox] = process.argv.slice(2);
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));

const relay = createRelay({ iceServers: [] }); // Players on one machine need no STUN
await listen(relay);
const relayUrl = process.env.TIDE_RELAY || `ws://127.0.0.1:${relay.address().port}`;
const relayOnly = process.env.TIDE_ICE_POLICY === 'relay';
// On a relay here, desktop players stay on loopback, so no port opens to the
// network, and Windows' firewall has nothing to ask. Except with Firefox on
// Windows, which offers no loopback candidates: there they take a port the
// system picks, as any game joining a match does.
const local = !process.env.TIDE_RELAY;
const env = { ...process.env, TIDE_RELAY: relayUrl, ...(relayOnly ? { TIDE_RTC_RELAY_ONLY: '1' } : {}) };
const loopback = { ...env, ...(local ? { TIDE_RTC_LOCAL: '1' } : {}) };

// A desktop player: its lines, as they come, and what it's told to do once
// what it waits for happened (see rooms_native.c)
function player(args, name, anywhere = false, more = {}) {
    const child = spawn(program, args, { env: { ...(anywhere ? env : loopback), ...more }, stdio: ['pipe', 'pipe', 'pipe'] });
    child.stdin.on('error', () => {}); // Gone already
    const lines = [];
    const waiting = [];
    let buffer = '';
    const take = chunk => {
        buffer += chunk;
        let at;
        while ((at = buffer.indexOf('\n')) >= 0) {
            const line = buffer.slice(0, at).trim();
            buffer = buffer.slice(at + 1);
            console.log(`${name}: ${line}`);
            lines.push(line);
            for (const w of [...waiting]) {
                if (w.test(line)) {
                    waiting.splice(waiting.indexOf(w), 1);
                    w.resolve(line);
                }
            }
        }
    };
    child.stdout.setEncoding('utf8').on('data', take);
    child.stderr.setEncoding('utf8').on('data', take);
    const exited = new Promise(resolve => child.on('exit', code => resolve(code)));
    const line = test => {
        const found = lines.find(l => test.test(l));
        if (found) return Promise.resolve(found);
        return new Promise(resolve => waiting.push({ test: l => test.test(l), resolve }));
    };
    const tell = what => child.stdin.write(`${what}\n`);
    return { child, line, exited, tell };
}

const within = (promise, seconds, what) => Promise.race([
    promise,
    new Promise((_, reject) => setTimeout(() => reject(new Error(`${what} took over ${seconds} seconds`)), seconds * 1000)),
]);

// The browser's page: a room's host or joiner that echoes, with the relay's protocol
const page = `<!doctype html><meta charset="utf-8"><title>rooms</title><script>
const q = new URLSearchParams(location.search);
const hosting = q.get('mode') === 'host';
const say = line => fetch('/say?browser=' + q.get('browser'), { method: 'POST', body: line });
say('loaded');
const ws = new WebSocket(q.get('relay'));
ws.onerror = () => say('relay error');
ws.onclose = e => say('relay closed ' + e.code + ' ' + e.reason);
const send = m => ws.send(JSON.stringify(m));
let ice = [];
const peers = new Map();
function channel(ch) {
  ch.binaryType = 'arraybuffer';
  ch.onopen = () => { say('open'); if (!hosting) ch.send(new Uint8Array([104, 105])); };
  ch.onmessage = e => ch.send(e.data);
}
function peer(id, signal) {
  const pc = new RTCPeerConnection({ iceServers: ice, iceTransportPolicy: q.get('policy') || 'all' });
  const p = { pc, chain: Promise.resolve() };
  pc.onicecandidate = e => { if (e.candidate) signal({ candidate: e.candidate.toJSON() }); };
  pc.ondatachannel = e => channel(e.channel);
  pc.onconnectionstatechange = () => say('state ' + pc.connectionState);
  pc.oniceconnectionstatechange = () => say('ice ' + pc.iceConnectionState);
  p.watch = () => {
    const t = pc.sctp && pc.sctp.transport;
    if (!t || p.watching) return;
    p.watching = true;
    t.onstatechange = () => say('dtls ' + t.state);
    t.onerror = e => say('dtls error ' + JSON.stringify({ detail: e.error && e.error.errorDetail,
      sent: e.error && e.error.sentAlert, received: e.error && e.error.receivedAlert, message: e.error && e.error.message }));
  };
  peers.set(id, p);
  return p;
}
function onSignal(p, s, answer) {
  p.chain = p.chain.then(async () => {
    if (s.description) {
      await p.pc.setRemoteDescription(s.description);
      p.watch();
      if (answer) {
        await p.pc.setLocalDescription();
        p.signal({ description: p.pc.localDescription.toJSON() });
      }
    } else if (s.candidate) {
      await p.pc.addIceCandidate(s.candidate);
    }
  }).catch(e => say('error ' + e.message));
}
ws.onmessage = async e => {
  const m = JSON.parse(e.data);
  if (m.relay) {
    ice = m.ice;
    send(hosting ? { host: q.get('code') } : { join: q.get('code') });
  } else if (m.hosting) {
    say('room ' + m.hosting);
  } else if (m.peer) {
    const p = peer(m.peer, s => send({ to: m.peer, signal: s }));
    p.signal = s => send({ to: m.peer, signal: s });
  } else if (m.from) {
    const p = peers.get(m.from);
    if (p) onSignal(p, m.signal, true);
  } else if (m.joined) {
    const p = peer(0, s => send({ signal: s }));
    channel(p.pc.createDataChannel('tide', { ordered: false, maxRetransmits: 0 }));
    await p.pc.setLocalDescription();
    send({ signal: { description: p.pc.localDescription.toJSON() } });
  } else if (m.signal) {
    onSignal(peers.get(0), m.signal, false);
  } else {
    say('relay ' + e.data);
  }
};
</script>`;

const said = [];
const waiting = [];
const server = createServer((request, response) => {
    if (request.url.startsWith('/page')) {
        response.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        response.end(page);
    } else if (request.url.startsWith('/say?') && request.method === 'POST') {
        const from = Number(new URL(request.url, 'http://127.0.0.1').searchParams.get('browser'));
        let body = '';
        request.on('data', chunk => { body += chunk; });
        request.on('end', () => {
            response.end();
            if (!running || from !== running.number) return; // A browser stopping, after its case
            console.log(`browser: ${body}`);
            said.push(body);
            for (const w of [...waiting]) {
                if (w.test.test(body)) {
                    waiting.splice(waiting.indexOf(w), 1);
                    w.resolve(body);
                }
            }
        });
    } else {
        response.writeHead(404);
        response.end();
    }
});
await listen(server);
const browserSaid = test => {
    const found = said.find(l => test.test(l));
    if (found) return Promise.resolve(found);
    return new Promise(resolve => waiting.push({ test, resolve }));
};

// Firefox's settings for a test: quiet, and like Chrome's flags below, with
// WebRTC using this machine's real addresses (both players are on it),
// unless it's to hide them behind .local names, as it does for players.
const FIREFOX_PREFS = `
user_pref("media.peerconnection.ice.loopback", true);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("app.update.enabled", false);
`;

// The browser running a case's page, which says when it's up: the case's own
// deadlines start there. Starting a browser takes far longer on a busy
// machine, or on one that never started it before (CI's first run, where a
// case's 45 seconds sometimes weren't enough for both), and isn't what's tested.
let running = null;
let browsers = 0;
function openBrowser(which, query) {
    // A folder of its own. Absolute, and there already: otherwise Chrome
    // shows a dialog about it, even headless.
    const number = ++browsers;
    const profile = resolve(`${scratch}-${process.pid}-${number}`);
    mkdirSync(profile, { recursive: true });
    const url = `http://127.0.0.1:${server.address().port}/page?relay=${encodeURIComponent(relayUrl)}`
        + `&policy=${relayOnly ? 'relay' : 'all'}&browser=${number}&${query}`;
    let child;
    if (which.firefox) {
        const hiding = `user_pref("media.peerconnection.ice.obfuscate_host_addresses", ${which.hidden});
`;
        writeFileSync(`${profile}/user.js`, FIREFOX_PREFS + hiding);
        // --wait-for-browser: on Windows, the process started is only a
        // launcher, and without it, stopping it would leave the browser
        child = spawn(which.path, ['--headless', '--no-remote', '--wait-for-browser', '--profile', profile, url],
            { stdio: 'ignore' });
    } else {
        // --remote-debugging-pipe: DevTools on fds 3 and 4, which stopBrowser
        // asks to close the browser
        child = spawn(which.path, [
            '--headless',
            '--no-first-run',
            '--no-default-browser-check',
            '--disable-extensions',
            '--remote-debugging-pipe',
            `--user-data-dir=${profile}`,
            ...(which.hidden ? [] : ['--disable-features=WebRtcHideLocalIpsWithMdns']),
            '--allow-loopback-in-peer-connection',
            url,
        ], { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'] });
        child.stdio[3].on('error', () => {}); // Gone already
        child.stdio[4].resume();
    }
    const exited = new Promise(resolve => child.on('exit', code => resolve(code)));
    running = { which, child, exited, number };
    const up = Promise.race([
        browserSaid(/^loaded$/),
        exited.then(code => { throw new Error(`the browser exited with ${code} before opening the page`); }),
    ]);
    return within(up, 90, 'the browser opening the page');
}

// The browser and everything it started, gone before the next case starts.
// Chrome closes itself: killing its processes from outside can miss one it
// starts again meanwhile, which then keeps the browser from going.
async function stopBrowser() {
    if (!running) return;
    const { which, child, exited } = running;
    running = null;
    const kill = () => {
        if (process.platform === 'win32') spawn('taskkill', ['/pid', String(child.pid), '/T', '/F'], { stdio: 'ignore' });
        else child.kill(which.firefox ? 'SIGTERM' : 'SIGKILL');
    };
    if (which.firefox) kill();
    else child.stdio[3].end(JSON.stringify({ id: 1, method: 'Browser.close' }) + '\0');
    try {
        await within(exited, 60, 'the browser stopping');
    } catch (error) {
        if (!which.firefox) kill();
        throw error;
    }
}

let failed = false;
const players = [];
async function test(name, run) {
    console.log(`--- ${name}`);
    said.length = 0;
    try {
        await run();
        console.log(`--- ${name}: ok`);
    } catch (error) {
        console.log(`--- ${name}: FAILED: ${error.message}`);
        failed = true;
    }
    for (const p of players.splice(0)) p.child.kill();
    try {
        await stopBrowser();
    } catch (error) {
        console.log(`--- ${name}: FAILED: ${error.message}`);
        failed = true;
    }
}

await test('two desktop players in a match', async () => {
    const host = player(['host'], 'host');
    players.push(host);
    const code = (await within(host.line(/^room /), 15, 'hosting')).split(' ')[1];
    const joiner = player(['join', code], 'joiner');
    players.push(joiner);
    await within(Promise.all([host.line(/^ok/), joiner.line(/^ok/)]), 45, 'meeting');
});

// Host migration: the host leaves, and its two players carry on, one of them
// hosting the room now, each the same player as before. It leaves once both
// said they're in, however long that took.
await test('a desktop match changes hands when its host leaves', async () => {
    const host = player(['handover-host'], 'host');
    players.push(host);
    const code = (await within(host.line(/^room /), 15, 'hosting')).split(' ')[1];
    const a = player(['handover-join', code, '2'], 'a');
    const b = player(['handover-join', code, '3'], 'b');
    players.push(a, b);
    const before = await within(Promise.all([a.line(/^ok before/), b.line(/^ok before/)]), 45, 'meeting');
    host.tell('leave');
    if ((await within(host.exited, 15, 'the host leaving')) !== 0) throw new Error('the host failed');
    const after = await within(Promise.all([a.line(/^ok after/), b.line(/^ok after/)]), 45, 'the match changing hands');
    const player_of = line => line.match(/player (\d+)/)[1];
    for (let i = 0; i < 2; i++) {
        if (player_of(before[i]) !== player_of(after[i])) throw new Error(`another player after: ${before[i]}, ${after[i]}`);
    }
    if (after.filter(line => line.endsWith('hosting')).length !== 1) throw new Error('not one of them hosts');
});

// A match its host ended stays ended, even for a player who missed the
// goodbye: the relay tells them so, rather than let them take the room over.
// The host ends it once the relay has its room, and quits at once, or, early,
// as soon as it has a room, which the relay may not know yet: then it goes on
// until it told the relay. TIDE_RTC_DEBUG has it say both: what the relay
// answered it, and what it told the relay.
for (const early of [false, true]) {
    await test(`a match its host ended${early ? ' before the relay knew its room' : ''} isn't taken over`, async () => {
        const host = player([early ? 'end-host-early' : 'end-host'], 'host', false, { TIDE_RTC_DEBUG: '1' });
        players.push(host);
        const [, code, , key] = (await within(host.line(/^room /), 15, 'hosting')).split(' ');
        if (early) {
            const told = await within(host.line(/the relay its match ended$/), 15, 'the host telling the relay');
            if (!told.includes(': told the relay')) throw new Error('the host couldn\'t tell the relay its match ended');
        } else {
            await within(host.line(new RegExp(`room ${code}: the relay has it$`)), 15, 'the relay opening the room');
            host.tell('end');
            if ((await within(host.exited, 15, 'the host ending the match')) !== 0) throw new Error('the host failed');
        }
        // Once the relay heard (one here: another can't be asked), so the late player can't get there first
        const heard = async () => { while (local && !relay.ended.has(code)) await new Promise(r => setTimeout(r, 20)); };
        await within(heard(), 15, 'the relay hearing the match ended');
        const late = player(['migrate', code, key], 'late');
        players.push(late);
        const answer = await within(late.line(/^migrated /), 30, 'the relay answering');
        if (answer !== 'migrated -2') throw new Error(`the relay said ${answer}, not that the match ended`);
    });
}

// Chrome (or Edge) and Firefox: two WebRTC implementations of their own.
// Each shows its address, then hides it behind a .local name, as it does for
// players: the desktop side then learns where it is from the checks it sends
// (on the network, not loopback).
const found = [
    { name: 'Chrome', path: browser, firefox: false, hidden: false },
    { name: 'Firefox', path: firefox, firefox: true, hidden: false },
    { name: 'Chrome, hiding its address,', path: browser, firefox: false, hidden: true },
    { name: 'Firefox, hiding its address,', path: firefox, firefox: true, hidden: true },
].filter(b => b.path && existsSync(b.path));
for (const which of found) {
    const anywhere = which.hidden || (which.firefox && process.platform === 'win32');
    await test(`${which.name} joins a desktop room`, async () => {
        const host = player(['echo-host'], 'desktop', anywhere);
        players.push(host);
        const code = (await within(host.line(/^room /), 15, 'hosting')).split(' ')[1];
        await openBrowser(which, `mode=join&code=${code}`);
        const exit = await within(host.exited, 45, 'the echoes');
        if (exit !== 0) throw new Error(`the desktop side exited with ${exit}`);
    });

    await test(`a desktop player joins a room ${which.name} hosts`, async () => {
        const code = 'B' + Math.random().toString(36).slice(2, 7).toUpperCase().replace(/[01IO]/g, '2');
        await openBrowser(which, `mode=host&code=${code}`);
        await within(browserSaid(/^room /), 30, 'hosting');
        const joiner = player(['echo-join', code], 'desktop', anywhere);
        players.push(joiner);
        const exit = await within(joiner.exited, 45, 'the echoes');
        if (exit !== 0) throw new Error(`the desktop side exited with ${exit}`);
    });
}
if (!found.length) console.log('No browser found: skipping the browser tests. Set TIDE_BROWSER to run them.');

// Last, this run's browser folders, which the browsers let go of as they stopped
relay.close();
server.close();
for (let i = 1; i <= browsers; i++) {
    try {
        rmSync(resolve(`${scratch}-${process.pid}-${i}`), { recursive: true, force: true });
    } catch {
        // Still held: it goes with the build folder
    }
}
process.exit(failed ? 1 : 0);
