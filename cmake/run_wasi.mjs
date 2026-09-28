// Runs a WebAssembly program under Node's WASI, the way CTest runs web builds'
// tests: node run_wasi.mjs <program.wasm> [arguments...]
// The program's exit code is Node's. The working directory is visible to it as
// ".", for tests that read or write files.

import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';

const [program, ...args] = process.argv.slice(2);
const wasi = new WASI({
    version: 'preview1',
    args: [program, ...args],
    env: {},
    preopens: { '.': process.cwd() },
    returnOnExit: true,
});
const module = await WebAssembly.compile(await readFile(program));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
process.exitCode = wasi.start(instance);
