// The site in production (see fly.toml): SvelteKit's handler, which serves the
// prerendered pages, the demos and the assets, with headers of our own on
// every response (headers.js).
//
// Run `npm run build`, then `node server.js [port]`; PORT sets the port too
// (3000).

import http from 'node:http';
import { handler } from './build/handler.js';
import { headers } from './headers.js';

const canonical = 'tide-engine.dev';

const server = http.createServer((req, res) => {
    // www. goes to the name without it.
    const host = req.headers.host ?? '';
    if (host === `www.${canonical}`) {
        res.writeHead(301, { location: `https://${canonical}${req.url}` }).end();
        return;
    }
    for (const [name, value] of Object.entries(headers)) res.setHeader(name, value);
    handler(req, res, () => {
        res.writeHead(404, { 'content-type': 'text/plain' }).end('Not found');
    });
});

const port = Number(process.argv[2] ?? process.env.PORT ?? 3000);
server.listen(port, () => console.log(`Listening on http://localhost:${port}`));

// Fly stops machines with SIGINT; finish what's in flight.
for (const signal of ['SIGINT', 'SIGTERM']) {
    process.on(signal, () => server.close(() => process.exit(0)));
}
