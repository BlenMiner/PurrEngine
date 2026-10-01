// Ticks on threads in headless Chrome or Edge: serves web_threads.c's page
// cross-origin isolated (the headers below), so it shares its memory with
// workers, and passes once the page exits with 0. In real time, rather than
// the other web tests' virtual time, which workers don't keep to.
//
//   node web_threads.mjs <browser> <tide_platform_web_threads.html> <scratch folder>

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, rmSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { resolve } from 'node:path';

const [browser, gamePage, profile] = process.argv.slice(2);
if (!browser || !existsSync(browser)) {
    console.log('SKIPPED: no Chrome or Edge found. Set TIDE_BROWSER to run web tests.');
    process.exit(0);
}
const page = await readFile(gamePage);

let finish;
const result = new Promise(resolve => { finish = resolve; });
const body = request => new Promise(resolve => {
    let text = '';
    request.on('data', chunk => { text += chunk; });
    request.on('end', () => resolve(text));
});
const server = createServer(async (request, response) => {
    if (request.url === '/') {
        response.writeHead(200, {
            'content-type': 'text/html; charset=utf-8',
            // What makes a page cross-origin isolated, so it can share memory
            'cross-origin-opener-policy': 'same-origin',
            'cross-origin-embedder-policy': 'require-corp',
        });
        response.end(page);
    } else if (request.url === '/log' && request.method === 'POST') {
        console.log(await body(request));
        response.end();
    } else if (request.url === '/result' && request.method === 'POST') {
        const code = await body(request);
        response.end();
        finish(code);
    } else {
        response.writeHead(404);
        response.end();
    }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));

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
    `http://127.0.0.1:${server.address().port}/`,
], { stdio: 'ignore' });
child.on('error', error => {
    console.log(`${browser}: ${error.message}`);
    finish('no browser');
});
const timeout = setTimeout(() => finish('the page said nothing more for 60 seconds'), 60000);

const code = await result;
clearTimeout(timeout);
child.kill();
console.log(code === '0' ? 'Ticks on threads in the browser came out the same as on one.' : `The page ended with ${code}.`);
process.exit(code === '0' ? 0 : 1);
