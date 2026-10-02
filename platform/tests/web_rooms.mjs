// Two players in a room, in headless Chrome or Edge: a relay on this machine
// (relay/relay.mjs), and a page with two frames running web_rooms.c's
// program, one hosting and one joining with the code the host says. Their
// packets go over WebRTC, as between two players' browsers. Once they meet,
// the host's page goes hidden for a while, as when its player switches tabs,
// and the match goes on. Passes once both say "ok".
//
//   node web_rooms.mjs <browser> <tide_platform_web_rooms.html> <scratch folder> [handover]
//
// With `handover`, three frames check host migration: the host leaves once
// both players joined, and passes once both say "ok after", each the same
// player as before, one of them hosting the room now. With `ended`, a host
// ends its match as soon as it has a room, which the relay may not know yet,
// and a frame that goes to its room again once the relay heard, as a player
// who missed the goodbye, has to hear from the relay that it ended.
//
// With TIDE_RELAY set, like wss://relay.tide-engine.dev, the players meet
// through that relay instead, to check a deployed one. TIDE_ICE_POLICY=relay
// sends their packets through its TURN servers, as for players whose networks
// can't connect directly.

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, rmSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { resolve } from 'node:path';

import { createRelay } from '../../relay/relay.mjs';

const [browser, gamePage, profile, mode] = process.argv.slice(2);
const handover = mode === 'handover';
const ended = mode === 'ended';
if (!browser || !existsSync(browser)) {
    console.log('SKIPPED: no Chrome or Edge found. Set TIDE_BROWSER to run web tests.');
    process.exit(0);
}

const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));

const relay = createRelay({ iceServers: [] }); // Players on one machine need no STUN
await listen(relay);
const relayUrl = process.env.TIDE_RELAY || `ws://127.0.0.1:${relay.address().port}`;
const game = await readFile(gamePage);

const page = `<!doctype html>
<meta charset="utf-8">
<title>Tide rooms test</title>
<body>
<script>
  const relay = ${JSON.stringify(relayUrl)};
  const handover = ${JSON.stringify(handover)};
  const ended = ${JSON.stringify(ended)};
  let hosted = null; // Its code and key, with \`ended\`
  const policy = ${JSON.stringify(process.env.TIDE_ICE_POLICY || 'all')};
  function player(who, args) {
    const frame = document.createElement('iframe');
    frame.src = 'game.html?who=' + who + '&relay=' + encodeURIComponent(relay) + '&policy=' + policy + '&args=' + args;
    document.body.append(frame);
  }
  const log = [];
  const ok = new Set();
  const before = {}, after = {};
  let reported = false;
  function report(passed) {
    if (reported) return;
    reported = true;
    fetch('result', { method: 'POST', body: JSON.stringify({ passed, log }) });
  }
  addEventListener('message', ({ data }) => {
    log.push(data.who + ': ' + data.line);
    fetch('say', { method: 'POST', body: data.who + ': ' + data.line }); // As it happens, in case the page hangs
    const keyed = /^room ([0-9A-Z]{6}) key ([0-9a-f]+)$/.exec(data.line);
    if (keyed) hosted = keyed;
    if (ended && data.who === 'host' && data.line === 'ended') {
      fetch('heard?code=' + hosted[1]).then(r => r.text()).then(heard => {
        if (heard === 'yes') player('late', 'migrate,' + hosted[1] + ',' + hosted[2]);
        else log.push('the relay never heard the match ended'), report(false);
      });
    }
    if (ended && data.who === 'late' && data.line.startsWith('migrated')) {
      if (data.line !== 'migrated -2') log.push('the relay never said the match ended');
      report(data.line === 'migrated -2');
    }
    const room = /^room ([0-9A-Z]{6})$/.exec(data.line);
    if (room && data.who === 'host' && !handover) player('joiner', 'join,' + room[1]);
    if (room && data.who === 'host' && handover) {
      player('a', 'handover-join,' + room[1] + ',2');
      player('b', 'handover-join,' + room[1] + ',3');
    }
    const number = /player ([0-9]+)/.exec(data.line);
    if (handover && data.line.startsWith('ok before')) before[data.who] = number[1];
    if (handover && data.line.startsWith('ok after')) {
      after[data.who] = { player: number[1], hosting: data.line.endsWith('hosting') };
      const done = Object.keys(after);
      if (done.length === 2) {
        const same = done.every(w => before[w] === after[w].player);
        const hosts = done.filter(w => after[w].hosting).length;
        if (!same) log.push('another player after the match changed hands');
        if (hosts !== 1) log.push(hosts + ' of them host the room');
        report(same && hosts === 1);
      }
    }
    if (!handover && data.line.startsWith('ok')) {
      ok.add(data.who);
      if (ok.size === 2) report(true);
    }
    if (/^(FAIL|exit|abort)/.test(data.line)) report(false);
  });
  player('host', ended ? 'end-host' : handover ? 'handover-host' : 'host');
</script>
`;

const said = []; // Every line, as it came
let finish;
const result = new Promise(resolve => { finish = resolve; });
const server = createServer((request, response) => {
    if (request.url === '/') {
        response.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        response.end(page);
    } else if (request.url.startsWith('/game.html')) {
        response.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        response.end(game);
    } else if (request.url === '/say' && request.method === 'POST') {
        let body = '';
        request.on('data', chunk => { body += chunk; });
        request.on('end', () => {
            response.end();
            said.push(body);
        });
    } else if (request.url.startsWith('/heard?')) {
        // Whether the relay heard the match in room `code` ended, once it did
        // (or 15 seconds on), so the late player can't get there first. A
        // relay elsewhere can't be asked: yes at once.
        const code = new URL(request.url, 'http://here').searchParams.get('code');
        const asked = Date.now();
        const answer = () => {
            if (process.env.TIDE_RELAY || relay.ended.has(code)) response.end('yes');
            else if (Date.now() - asked > 15000) response.end('no');
            else setTimeout(answer, 20);
        };
        answer();
    } else if (request.url === '/result' && request.method === 'POST') {
        let body = '';
        request.on('data', chunk => { body += chunk; });
        request.on('end', () => {
            response.end();
            finish(JSON.parse(body));
        });
    } else {
        response.writeHead(404);
        response.end();
    }
});
await listen(server);

// Absolute, and there already: otherwise Chrome shows a dialog about it, even headless
const profileDir = resolve(profile);
rmSync(profileDir, { recursive: true, force: true });
mkdirSync(profileDir, { recursive: true });
const child = spawn(browser, [
    '--headless',
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-extensions',
    `--user-data-dir=${profileDir}`,
    '--use-angle=swiftshader',
    '--enable-unsafe-swiftshader',
    // Both players are on this machine: let WebRTC use its real addresses
    '--disable-features=WebRtcHideLocalIpsWithMdns',
    '--allow-loopback-in-peer-connection',
    `http://127.0.0.1:${server.address().port}/`,
], { stdio: 'ignore' });
child.on('error', error => finish({ passed: false, log: [`${browser}: ${error.message}`] }));
const timeout = setTimeout(() => finish({ passed: false, log: [...said, 'the page said nothing more for 60 seconds'] }), 60000);

const { passed, log } = await result;
clearTimeout(timeout);
child.kill();
for (const line of log) console.log(line);
if (ended) console.log(passed ? 'The match stayed ended.' : 'The match didn\'t stay ended.');
else if (handover) console.log(passed ? 'The match changed hands.' : 'The match didn\'t change hands.');
else console.log(passed ? 'Both players met in a room, and played on while the host\'s page was hidden.'
                        : 'The players didn\'t meet, or the match didn\'t go on while the host\'s page was hidden.');
process.exit(passed ? 0 : 1);
