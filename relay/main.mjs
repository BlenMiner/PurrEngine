// Runs the relay (relay.mjs): node main.mjs [port], or $PORT, 8080 without one.
// On Fly (fly.toml), players' addresses come from its proxy, and with the
// secrets CLOUDFLARE_TURN_KEY_ID and CLOUDFLARE_TURN_API_TOKEN, players who
// can't connect directly go through Cloudflare's TURN.

import { cloudflareTurn, createRelay, DEFAULT_ICE } from './relay.mjs';

const port = Number(process.argv[2] || process.env.PORT || 8080);
const keyId = process.env.CLOUDFLARE_TURN_KEY_ID;
const token = process.env.CLOUDFLARE_TURN_API_TOKEN;
const iceServers = keyId && token ? cloudflareTurn(keyId, token) : DEFAULT_ICE;
const relay = createRelay({ iceServers, trustProxy: !!process.env.FLY_APP_NAME });
relay.listen(port, () => console.log(`relay: listening on port ${port}, ${keyId && token ? 'with' : 'without'} TURN`));

// Fly stops machines with SIGINT or SIGTERM: rooms end with their connections.
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => process.exit(0));
