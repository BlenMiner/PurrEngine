// Serves a web build's test page to headless Chrome or Edge in real time, and
// passes once the page exits with 0. For tests of WebGPU (see WebTest.cmake):
// the browser's answer about its GPU doesn't come under the other web tests'
// virtual time. The page's shell sends what the program prints and how it
// ends back here, as platform/web/test_shell.html does when it's served.
//
// The page is cross-origin isolated (the headers below), so the program runs
// on threads, sharing its memory with them. WebGPU is the browser's software
// one, so no GPU is needed: SwiftShader, which the flags below turn on.
//
//   node run_web_served_test.mjs <browser> <page.html> <scratch folder> [<query>] [--expect <text>]

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, rmSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { resolve } from 'node:path';

const [browser, pagePath, profile, ...rest] = process.argv.slice(2);
const expectAt = rest.indexOf('--expect');
const expected = expectAt >= 0 ? rest[expectAt + 1] : null; // A line the program has to print
const query = rest.length && expectAt !== 0 ? rest[0] : '';
if (!browser || !existsSync(browser)) {
    console.log('SKIPPED: no Chrome or Edge found. Set TIDE_BROWSER to run web tests.');
    process.exit(0);
}
const page = await readFile(pagePath);

let finish;
const result = new Promise(done => { finish = done; });
const body = request => new Promise(done => {
    let text = '';
    request.on('data', chunk => { text += chunk; });
    request.on('end', () => done(text));
});
let seen = !expected;
const server = createServer(async (request, response) => {
    const path = request.url.split('?')[0];
    if (path === '/') {
        response.writeHead(200, {
            'content-type': 'text/html; charset=utf-8',
            // What makes a page cross-origin isolated, so it can share memory
            'cross-origin-opener-policy': 'same-origin',
            'cross-origin-embedder-policy': 'require-corp',
        });
        response.end(page);
    } else if (path === '/log' && request.method === 'POST') {
        const line = await body(request);
        console.log(line);
        if (expected && line.includes(expected)) seen = true;
        response.end();
    } else if (path === '/result' && request.method === 'POST') {
        const code = await body(request);
        response.end();
        finish(code);
    } else {
        response.writeHead(404);
        response.end();
    }
});
await new Promise(done => server.listen(0, '127.0.0.1', done));

// WebGPU in software, and on Windows the browser's own drawing through WARP:
// with SwiftShader for that too (--use-angle=swiftshader, as the other web
// tests run), Chrome has no WebGPU at all. TIDE_WEBGPU_FLAGS has other flags
// tried in their place, like "--enable-unsafe-webgpu" alone for this
// machine's GPU.
const flags = ['--enable-unsafe-webgpu', '--use-webgpu-adapter=swiftshader'];
if (process.platform === 'win32') flags.push('--use-angle=d3d11-warp');
if (process.env.TIDE_WEBGPU_FLAGS) flags.splice(0, flags.length, ...process.env.TIDE_WEBGPU_FLAGS.split(' '));

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
    ...flags,
    `http://127.0.0.1:${server.address().port}/${query ? '?' + query : ''}`,
], { stdio: 'ignore' });
child.on('error', error => {
    console.log(`${browser}: ${error.message}`);
    finish('no browser');
});
const timeout = setTimeout(() => finish('no result within 100 seconds'), 100000);

const code = await result;
clearTimeout(timeout);
child.kill();
if (code === '0' && !seen) console.log(`The page never printed "${expected}".`);
else if (code !== '0') console.log(`The page ended with: ${code}.`);
process.exit(code === '0' && seen ? 0 : 1);
