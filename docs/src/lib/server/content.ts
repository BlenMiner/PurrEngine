// The docs' pages, from the Markdown files in docs/. Every page is
// prerendered, so this only runs at build time (and in `npm run dev`).

import fs from 'node:fs';
import path from 'node:path';
import { parse } from 'yaml';
import { pages, repo, type Link } from '#lib/nav.ts';
import { language, render, type Heading } from './markdown.ts';

const sources = import.meta.glob(['/**/*.md', '!/node_modules/**', '!/.svelte-kit/**', '!/build/**', '!/static/**'], {
    query: '?raw',
    import: 'default',
    eager: true,
}) as Record<string, string>;

/** `/guide/index.md` is `/guide`, `/spec.md` is `/spec`. */
function pathOf(file: string): string {
    return file.replace(/\.md$/, '').replace(/\/index$/, '') || '/';
}

const files = new Map(Object.keys(sources).map((file) => [pathOf(file), file]));

// A page missing from the sidebar couldn't be reached, and a sidebar link
// without a page would be dead.
const problems = [
    ...[...files.keys()].filter((p) => p !== '/' && !pages.some((page) => page.link === p)).map((p) => `${files.get(p)} isn't in the sidebar (src/lib/nav.ts)`),
    ...pages.filter((page) => !files.has(page.link)).map((page) => `the sidebar's ${page.link} has no Markdown file`),
];
if (problems.length) throw new Error(`The docs' pages don't match their sidebar:\n  ${problems.join('\n  ')}`);

const isPage = (p: string) => files.has(p);

function frontmatter(source: string): { data: Record<string, unknown>; body: string } {
    const match = /^---\r?\n([\s\S]*?)\r?\n---\r?\n/.exec(source);
    return match ? { data: parse(match[1]) ?? {}, body: source.slice(match[0].length) } : { data: {}, body: source };
}

// `<<< @/../demo/demo.tide` shows a file of the repo as code (`@` is docs/),
// as VitePress did.
function snippets(source: string, file: string): string {
    return source.replace(/^<<< @\/(\S+)[ \t]*\r?$/gm, (_, relative: string) => {
        const where = path.resolve(relative);
        if (!fs.existsSync(where)) throw new Error(`${file}: <<< @/${relative}: no such file`);
        const code = fs.readFileSync(where, 'utf8').replace(/\r\n/g, '\n').replace(/\n$/, '');
        const longest = Math.max(0, ...(code.match(/^`+/gm) ?? []).map((run) => run.length));
        const fence = '`'.repeat(Math.max(3, longest + 1));
        return `${fence}${language(path.extname(where).slice(1))}\n${code}\n${fence}`;
    });
}

function renderFile(file: string) {
    const { data, body } = frontmatter(sources[file]);
    return { data, ...render(snippets(body, file), file.slice(1), isPage) };
}

/** A sentence or two from the start of the page, for search engines. */
function describe(text: string): string {
    if (text.length <= 160) return text;
    const cut = text.slice(0, 160);
    return `${cut.slice(0, cut.lastIndexOf(' '))}…`;
}

export interface Page {
    path: string;
    title: string;
    description: string;
    html: string;
    headings: Heading[];
    edit: string;
    prev?: Link;
    next?: Link;
}

const cache = new Map<string, Page>();

export function page(slug: string): Page {
    const at = `/${slug}`;
    let result = cache.get(at);
    if (!result) {
        const file = files.get(at)!;
        const rendered = renderFile(file);
        const index = pages.findIndex((p) => p.link === at);
        result = {
            path: at,
            title: rendered.title || pages[index].text,
            description: describe(rendered.sections[0].text),
            html: rendered.html,
            headings: rendered.headings,
            edit: `${repo}/edit/dev/docs${file}`,
            prev: pages[index - 1],
            next: pages[index + 1],
        };
        cache.set(at, result);
    }
    return result;
}

export interface Home {
    hero: {
        name: string;
        text: string;
        tagline: string;
        actions: { theme: 'brand' | 'alt'; text: string; link: string }[];
    };
    features: { title: string; details: string }[];
    html: string;
}

/** The home page: `index.md`, its hero and features in its front matter. */
export function home(): Home {
    const { data, html } = renderFile('/index.md');
    return { hero: data.hero as Home['hero'], features: data.features as Home['features'], html };
}

export interface Entry {
    /** The page's path, and the section's anchor. */
    id: string;
    titles: string[];
    text: string;
}

/** What search looks through: every page, a section per heading. */
export function searchIndex(): Entry[] {
    return [...files.entries()]
        .filter(([at]) => at !== '/')
        .flatMap(([at, file]) =>
            renderFile(file).sections.map((section) => ({ id: section.id ? `${at}#${section.id}` : at, titles: section.titles, text: section.text })),
        );
}
