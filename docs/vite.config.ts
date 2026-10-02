// The docs site: SvelteKit, every page prerendered from the Markdown next to
// this file, and served on Fly.io by server.js (see fly.toml).
// `npm run dev` serves it locally.

import fs from 'node:fs';
import { sveltekit } from '@sveltejs/kit/vite';
import adapter from '@sveltejs/adapter-node';
import { defineConfig, type Plugin } from 'vite';
import { headers } from './headers.js';

// `npm run dev` serves what server.js does in production: the site's headers
// on every response, and static/sand/index.html at /sand/.
const asInProduction: Plugin = {
    name: 'as-in-production',
    configureServer(server) {
        server.middlewares.use((req, res, next) => {
            for (const [name, value] of Object.entries(headers)) res.setHeader(name, value);
            const [path, query] = (req.url ?? '').split('?');
            if (path.endsWith('/') && path !== '/' && fs.existsSync(`static${path}index.html`)) {
                req.url = `${path}index.html${query === undefined ? '' : `?${query}`}`;
            }
            next();
        });
    },
};

export default defineConfig(({ command }) => ({
    plugins: [
        asInProduction,
        sveltekit({
            adapter: adapter(),
            prerender: {
                handleHttpError: ({ path, message }) => {
                    // Sand (static/sand/) is built by CMake and copied in; a
                    // build without it still works.
                    if (path === '/sand/') return;
                    throw new Error(message);
                },
            },
            // Warnings are errors, as everywhere in the repo.
            onwarn: (warning, handler) => {
                if (command === 'build') throw new Error(`${warning.filename}:${warning.start?.line}: ${warning.message}`);
                handler(warning);
            },
        }),
    ],
    // Pages show code from the rest of the repo (`<<<`), and the grammar
    // comes from tools/tide-syntax.
    server: { fs: { allow: ['..'] } },
}));
