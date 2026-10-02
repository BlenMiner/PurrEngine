import { defineParams } from '@sveltejs/kit/params';
import { slugs } from '#lib/nav.ts';

export const params = defineParams({
    // Only the docs' own pages: anything else, like the demos in static/,
    // loads as a page of its own rather than through the router.
    doc: (param) => (slugs.has(param) ? param : undefined),
});
