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

// A desktop player: its lines, as they come
function player(args, name, anywhere = false) {
    const child = spawn(program, args, { env: anywhere ? env : loopback, stdio: ['ignore', 'pipe', 'pipe'] });
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
    return { child, line, exited };
}

const within = (promise, seconds, what) => Promise.race([
    promise,
    new Promise((_, reject) => setTimeout(() => reject(new Error(`${what} took over ${seconds} seconds`)), seconds * 1000)),
]);

// The browser's page: a room's host or joiner that echoes, with the relay's protocol
const page = `<!doctype html><meta charset="utf-8"><title>rooms</title><script>
const q = new URLSearchParams(location.search);
const hosting = q.get('mode') === 'host';
const say = line => fetch('/say', { method: 'POST', body: line });
const ws = new WebSocket(q.get('relay'));
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
    } else if (request.url === '/say' && request.method === 'POST') {
        let body = '';
        request.on('data', chunk => { body += chunk; });
        request.on('end', () => {
            response.end();
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

let running = null;
let browsers = 0;
function openBrowser(which, query) {
    // A folder of its own, since a browser just stopped may still hold the
    // last one. Absolute, and there already: otherwise Chrome shows a dialog
    // about it, even headless.
    const profile = resolve(`${scratch}-${process.pid}-${++browsers}`);
    mkdirSync(profile, { recursive: true });
    const url = `http://127.0.0.1:${server.address().port}/page?relay=${encodeURIComponent(relayUrl)}`
        + `&policy=${relayOnly ? 'relay' : 'all'}&${query}`;
    if (which.firefox) {
        const hiding = `user_pref("media.peerconnection.ice.obfuscate_host_addresses", ${which.hidden});
`;
        writeFileSync(`${profile}/user.js`, FIREFOX_PREFS + hiding);
        // --wait-for-browser: on Windows, the process started is only a
        // launcher, and without it, stopping it would leave the browser
        running = spawn(which.path, ['--headless', '--no-remote', '--wait-for-browser', '--profile', profile, url],
            { stdio: 'ignore' });
    } else {
        running = spawn(which.path, [
            '--headless',
            '--no-first-run',
            '--no-default-browser-check',
            '--disable-extensions',
            `--user-data-dir=${profile}`,
            ...(which.hidden ? [] : ['--disable-features=WebRtcHideLocalIpsWithMdns']),
            '--allow-loopback-in-peer-connection',
            url,
        ], { stdio: 'ignore' });
    }
}

// The browser and everything it started
function stopBrowser() {
    if (!running) return;
    if (process.platform === 'win32') spawn('taskkill', ['/pid', String(running.pid), '/T', '/F'], { stdio: 'ignore' });
    else running.kill();
    running = null;
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
    stopBrowser();
}

await test('two desktop players in a match', async () => {
    const host = player(['host'], 'host');
    players.push(host);
    const code = (await within(host.line(/^room /), 15, 'hosting')).split(' ')[1];
    const joiner = player(['join', code], 'joiner');
    players.push(joiner);
    await within(Promise.all([host.line(/^ok/), joiner.line(/^ok/)]), 45, 'meeting');
});

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
        openBrowser(which, `mode=join&code=${code}`);
        const exit = await within(host.exited, 45, 'the echoes');
        if (exit !== 0) throw new Error(`the desktop side exited with ${exit}`);
    });

    await test(`a desktop player joins a room ${which.name} hosts`, async () => {
        const code = 'B' + Math.random().toString(36).slice(2, 7).toUpperCase().replace(/[01IO]/g, '2');
        openBrowser(which, `mode=host&code=${code}`);
        await within(browserSaid(/^room /), 30, 'hosting');
        const joiner = player(['echo-join', code], 'desktop', anywhere);
        players.push(joiner);
        const exit = await within(joiner.exited, 45, 'the echoes');
        if (exit !== 0) throw new Error(`the desktop side exited with ${exit}`);
    });
}
if (!found.length) console.log('No browser found: skipping the browser tests. Set TIDE_BROWSER to run them.');

// Last, this run's browser folders, once the browsers have let go of them
relay.close();
server.close();
await new Promise(r => setTimeout(r, 1000));
for (let i = 1; i <= browsers; i++) {
    try {
        rmSync(resolve(`${scratch}-${process.pid}-${i}`), { recursive: true, force: true });
    } catch {
        // Still held: it goes with the build folder
    }
}
process.exit(failed ? 1 : 0);
