// Makes a self-contained web page from a WebAssembly program:
//
//   node web_page.mjs <program.wasm> <shell.html> <purr.js> <page.html>
//
// The shell's {{{ SCRIPT }}} becomes a <script> with the program (base64) and
// purr.js, which runs it, so the page opens straight from disk. The purr
// command does the same for games (compiler/cli/build.c).

import { readFile, writeFile } from 'node:fs/promises';

const [program, shell, script, page] = process.argv.slice(2);
const wasm = (await readFile(program)).toString('base64');
const template = await readFile(shell, 'utf8');
if (!template.includes('{{{ SCRIPT }}}')) {
    console.error(`${shell} has no {{{ SCRIPT }}} for the program`);
    process.exit(1);
}
const tag = `<script>\nconst PURR_PROGRAM = "${wasm}";\n${await readFile(script, 'utf8')}</script>`;
await writeFile(page, template.replace('{{{ SCRIPT }}}', () => tag));
