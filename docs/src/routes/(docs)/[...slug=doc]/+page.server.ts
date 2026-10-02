import { slugs } from '#lib/nav.ts';
import { page } from '#lib/server/content.ts';

export const entries = () => [...slugs].map((slug) => ({ slug }));

export const load = ({ params }) => page(params.slug);
