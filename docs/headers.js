// Headers every response of the site goes out with, in development
// (vite.config.ts) and on Fly (server.js).
//
// The two Cross-Origin ones make pages cross-origin isolated, which lets the
// demos share their memory with the workers their threads run in: without
// them, a game runs on one thread. Every page has them, not only the demos',
// because a page in a frame is only isolated when the page around it is too,
// and moving between pages doesn't load a new one. So everything a page
// loads has to come from this site (or say it may be embedded, with CORP or
// CORS).
export const headers = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'strict-origin-when-cross-origin',
};
