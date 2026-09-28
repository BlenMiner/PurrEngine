// Runs a headless browser that dumps a page's DOM (see run_web_test.cmake),
// passing its output through, and stops it once the page is out: on macOS,
// Chrome doesn't exit by itself after --dump-dom.
//
// node browser_dump.mjs <browser> <arguments>...

import { spawn } from 'node:child_process';

const [browser, ...args] = process.argv.slice(2);
const child = spawn(browser, args, { stdio: ['ignore', 'pipe', 'inherit'] });
let dom = '';
child.stdout.setEncoding('utf8');
child.stdout.on('data', chunk => {
    process.stdout.write(chunk);
    dom += chunk;
    if (dom.includes('</html>')) child.kill();
});
child.on('error', error => {
    console.error(`${browser}: ${error.message}`);
    process.exitCode = 1;
});
