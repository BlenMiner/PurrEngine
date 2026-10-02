// Runs a WebAssembly program under Node's WASI, the way CTest runs web builds'
// tests: node run_wasi.mjs <program.wasm> [arguments...]
// The program's exit code is Node's. The working directory is visible to it as
// ".", for tests that read or write files.
//
// Programs import their memory, shared between their threads (see
// platform/web/tide.js, which does the same in pages): each thread the program
// starts (wasi-threads' thread-spawn) is a worker running it on that memory.

import { readFile } from 'node:fs/promises';
import { availableParallelism } from 'node:os';
import { WASI } from 'node:wasi';
import { Worker } from 'node:worker_threads';

const [program, ...args] = process.argv.slice(2);
const bytes = await readFile(program);

// The memory a program imports (env.memory): its limits and whether it's
// shared, from its import section.
function memoryImport(bytes) {
    let at = 8; // After the magic number and version
    const leb = () => {
        let value = 0, scale = 1, byte;
        do {
            byte = bytes[at++];
            value += (byte & 0x7f) * scale;
            scale *= 128;
        } while (byte & 0x80);
        return value;
    };
    const name = () => {
        const length = leb();
        at += length;
        return bytes.subarray(at - length, at).toString();
    };
    while (at < bytes.length) {
        const id = bytes[at++];
        const end = leb() + at;
        if (id !== 2) { // Not the imports
            at = end;
            continue;
        }
        for (let count = leb(); count > 0; count--) {
            const from = name(), field = name(), kind = bytes[at++];
            if (kind === 0) leb(); // A function: its type
            else if (kind === 3) at += 2; // A global: its type, and whether it changes
            else if (kind === 4) { at++; leb(); } // A tag
            else { // A table's or a memory's limits, after a table's element type
                if (kind === 1) at++;
                const flags = bytes[at++];
                const initial = leb(), maximum = flags & 1 ? leb() : undefined;
                if (kind === 2 && from === 'env' && field === 'memory') return { shared: (flags & 2) !== 0, initial, maximum };
            }
        }
        return null;
    }
    return null;
}

const wanted = memoryImport(bytes);
const memory = wanted && new WebAssembly.Memory({ initial: wanted.initial, maximum: wanted.maximum, shared: wanted.shared });
const module = await WebAssembly.compile(bytes);

// What each thread's worker runs: the program on the same memory, from
// wasi_thread_start, with WASI's output, clock and random numbers.
const threadSource = `
const { workerData: { module, memory, tid, startArg } } = require('node:worker_threads');
const { writeSync } = require('node:fs');
const { randomFillSync } = require('node:crypto');
const view = () => new DataView(memory.buffer);
const u8 = () => new Uint8Array(memory.buffer);
const wasi = {
    fd_write(fd, iovs, count, writtenPtr) {
        let written = 0;
        for (let i = 0; i < count; i++) {
            const ptr = view().getUint32(iovs + i * 8, true), len = view().getUint32(iovs + i * 8 + 4, true);
            if (fd === 1 || fd === 2) writeSync(fd, u8().slice(ptr, ptr + len));
            written += len;
        }
        view().setUint32(writtenPtr, written, true);
        return fd === 1 || fd === 2 ? 0 : 8;
    },
    clock_time_get(id, precision, timePtr) {
        view().setBigUint64(timePtr, id === 0 ? BigInt(Date.now()) * 1000000n : process.hrtime.bigint(), true);
        return 0;
    },
    random_get(ptr, len) {
        const bytes = new Uint8Array(len);
        randomFillSync(bytes);
        u8().set(bytes, ptr);
        return 0;
    },
    sched_yield: () => 0,
    proc_exit: code => process.exit(code),
};
const imports = {};
for (const { module: from, name, kind } of WebAssembly.Module.imports(module)) {
    imports[from] = imports[from] || {};
    if (kind === 'memory') imports[from][name] = memory;
    else if (from === 'wasi_snapshot_preview1') imports[from][name] = wasi[name] || (() => 52); // ENOSYS
    else imports[from][name] = () => { throw new Error(name + " only works on the program's own thread"); };
}
try {
    new WebAssembly.Instance(module, imports).exports.wasi_thread_start(tid, startArg);
} catch (error) {
    // The program's own thread may be spinning, waiting for this one, and never
    // get back to Node's loop to hear of it: say so here, and end the program
    // as a crash would
    writeSync(2, 'thread ' + tid + ': ' + (error && error.stack || error) + '\\n');
    process.kill(process.pid, 'SIGKILL');
}
`;

let nextThread = 1;
function spawnThread(startArg) {
    if (!memory || !wanted.shared) return -1;
    const tid = nextThread++;
    const worker = new Worker(threadSource, { eval: true, workerData: { module, memory, tid, startArg } });
    worker.on('error', error => {
        console.error(`thread ${tid}: ${error && error.stack || error}`);
        process.exit(1);
    });
    worker.unref(); // Threads wait for work forever: the program's end is the main thread's
    return tid;
}

const wasi = new WASI({
    version: 'preview1',
    args: [program, ...args],
    env: {},
    preopens: { '.': process.cwd() },
    returnOnExit: true,
});
const imports = {
    ...wasi.getImportObject(),
    env: { memory },
    wasi: { 'thread-spawn': spawnThread },
    // What the page gives web programs that tests can use (platform/web/tide_web.h)
    tide: { threads: () => (memory && wanted.shared ? Math.min(availableParallelism(), 32) : 1) },
};
const instance = await WebAssembly.instantiate(module, imports);
process.exitCode = wasi.start(instance);
