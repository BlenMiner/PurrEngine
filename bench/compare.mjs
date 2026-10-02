// Compares saved benchmark runs (bench_<name> --save <label>, which adds to
// bench/results/<name>.jsonl): runs with the same options, on the same kind
// of machine, side by side, each against the first one that passed.
//
//   node bench/compare.mjs [benchmark...] [--last <n>]

import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const results = join(dirname(fileURLToPath(import.meta.url)), "results");

const args = process.argv.slice(2);
let last = 6;
const names = [];
for (let i = 0; i < args.length; i++) {
    if (args[i] === "--last") last = Number(args[++i]);
    else names.push(args[i]);
}

// What's compared: where it is in a record, what it's called, and how it's shown.
const ms = (v) => v.toFixed(2);
const metrics = [
    ["server.update_ms.median", "server update, median (ms)", ms],
    ["server.update_ms.p99", "server update, p99 (ms)", ms],
    ["client.update_ms.median", "client update, median (ms)", ms],
    ["client.update_ms.p99", "client update, p99 (ms)", ms],
    ["server.simulate_ms", "server simulating (ms/s)", ms],
    ["server.hash_ms", "server hashing (ms/s)", ms],
    ["client.simulate_ms", "client simulating (ms/s)", ms],
    ["client.snapshot_ms", "client snapshots (ms/s)", ms],
    ["client.hash_ms", "client hashing (ms/s)", ms],
    ["replays", "client ticks per tick played", (v) => v.toFixed(1)],
    ["server.sent_bytes", "server sends (KB/s)", (v) => (v / 1e3).toFixed(1)],
    ["client.sent_bytes", "client sends (KB/s)", (v) => (v / 1e3).toFixed(1)],
    ["caught_up_s.max", "slowest join (s)", ms],
    ["pack_ms_each", "packing the world (ms)", ms],
    ["world_sent_bytes", "world as sent (MB)", (v) => (v / 1e6).toFixed(2)],
    ["world_bytes", "world in memory (MB)", (v) => (v / 1e6).toFixed(2)],
];

const at = (record, path) => path.split(".").reduce((v, key) => (v == null ? undefined : v[key]), record);
const pad = (text, width) => String(text).padStart(width);

const files = readdirSync(results).filter((f) => f.endsWith(".jsonl"));
for (const file of files) {
    const benchmark = file.slice(0, -".jsonl".length);
    if (names.length && !names.includes(benchmark)) continue;
    const records = readFileSync(join(results, file), "utf8")
        .split("\n")
        .filter((line) => line.trim())
        .map((line) => JSON.parse(line));

    // Runs compare when they ran the same thing on the same kind of machine
    const groups = new Map();
    for (const r of records) {
        const key = JSON.stringify([r.options, r.os, r.optimized, r.threads]);
        if (!groups.has(key)) groups.set(key, []);
        groups.get(key).push(r);
    }
    for (const runs of groups.values()) {
        const shown = runs.slice(-last);
        const first = shown[0];
        const base = shown.find((r) => r.ok); // What changes are against: a run that failed measured something else
        const options = Object.entries(first.options).map(([k, v]) => `${k} ${v}`).join(", ");
        console.log(`\n${benchmark}: ${options}; ${first.os}, ${first.threads} threads${first.optimized ? "" : ", unoptimized"}`);
        const width = 22;
        console.log(pad("", 32) + shown.map((r) => pad(r.label, width)).join(""));
        console.log(pad("", 32) + shown.map((r) => pad(r.date.slice(0, 10), width)).join(""));
        for (const [path, name, show] of metrics) {
            const from = base ? at(base, path) : undefined;
            const cells = shown.map((r) => {
                const v = at(r, path);
                if (v == null || (!v && !r.ok)) return pad("-", width);
                if (r === base || !r.ok || !from) return pad(show(v), width);
                const change = Math.round(((v - from) / from) * 100);
                return pad(`${show(v)} (${change > 0 ? "+" : ""}${change}%)`, width);
            });
            console.log(name.padEnd(32) + cells.join(""));
        }
        console.log("ok".padEnd(32) + shown.map((r) => pad(r.ok ? "yes" : "FAILED", width)).join(""));
    }
}
