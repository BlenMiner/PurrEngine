import { json } from '@sveltejs/kit';
import { searchIndex } from '#lib/server/content.ts';

// What the search box looks through, loaded the first time it opens. Built
// with the pages: no link leads here for the build to find it by.
export const prerender = true;

export const GET = () => json(searchIndex());
