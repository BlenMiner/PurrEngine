// Markdown to HTML, as GitHub shows the same files, plus what the docs need:
// highlighted code (Tide with the editors' grammar), heading anchors, links
// between pages, `::: tip` and the like, and `::: code-group` tabs.

import MarkdownIt, { type Env as MarkdownEnv, type RendererRule, type Token } from 'markdown-it';
import container from 'markdown-it-container';
import { createHighlighterCore } from 'shiki/core';
import { createOnigurumaEngine } from 'shiki/engine/oniguruma';
import grammar from '../../../../tools/tide-syntax/syntaxes/tide.tmLanguage.json' with { type: 'json' };

export interface Heading {
    level: number;
    id: string;
    /** The heading as HTML, for the outline: inline code stays code. */
    html: string;
}

export interface Section {
    /** The heading's anchor, empty for the top of the page. */
    id: string;
    /** The headings above it and its own, as plain text. */
    titles: string[];
    text: string;
}

export interface Rendered {
    title: string;
    html: string;
    headings: Heading[];
    sections: Section[];
}

interface Env extends MarkdownEnv {
    /** The Markdown file's path in docs/, like `guide/index.md`. */
    file: string;
    /** Whether a path is one of the site's pages. */
    isPage: (path: string) => boolean;
    /** Inside a `::: code-group`. */
    grouped: boolean;
    groups: number;
}

// Only the languages the docs show, rather than every one shiki has.
const highlighter = await createHighlighterCore({
    themes: [import('shiki/themes/github-light.mjs'), import('shiki/themes/github-dark.mjs')],
    langs: [
        { ...(grammar as object), name: 'tide' } as never,
        import('shiki/langs/c.mjs'),
        import('shiki/langs/shellscript.mjs'),
        import('shiki/langs/powershell.mjs'),
        import('shiki/langs/cmake.mjs'),
        import('shiki/langs/json.mjs'),
        import('shiki/langs/javascript.mjs'),
    ],
    engine: createOnigurumaEngine(import('shiki/wasm')),
});

// The repo's Markdown fences Tide as csharp, which GitHub highlights; the
// site has the real grammar.
const aliases: Record<string, string> = { csharp: 'tide', 'c#': 'tide', sh: 'shellscript', bash: 'shellscript', shell: 'shellscript', ps1: 'powershell' };
const labels: Record<string, string> = { shellscript: 'sh' };

/** The language a fence or a file extension names, if the highlighter has it. */
export function language(name: string): string {
    const lang = aliases[name] ?? name;
    return highlighter.getLoadedLanguages().includes(lang) ? lang : 'text';
}

/** Anchors as VitePress made them, so links into the old site still land. */
export function slugify(text: string): string {
    return text
        .normalize('NFKD')
        .replace(/\p{M}/gu, '')
        .replace(/[\u0000-\u001f]/g, '')
        .replace(/[\s~`!@#$%^&*()\-_+=[\]{}|\\;:"'“”‘’<>,.?/]+/g, '-')
        .replace(/-{2,}/g, '-')
        .replace(/^-+|-+$/g, '')
        .replace(/^(\d)/, '_$1')
        .toLowerCase();
}

const md = new MarkdownIt({ html: true });

function escape(text: string): string {
    return md.utils.escapeHtml(text);
}

function plain(inline: Token): string {
    return (inline.children ?? [])
        .map((child) => (child.type === 'text' || child.type === 'code_inline' ? child.content : child.type.endsWith('break') ? ' ' : ''))
        .join('');
}

md.renderer.rules.fence = (tokens, idx, _options, env) => {
    const token = tokens[idx];
    const lang = language(token.info.trim().split(/\s+/)[0] || 'text');
    const code = highlighter.codeToHtml(token.content.replace(/\n$/, ''), {
        lang,
        themes: { light: 'github-light', dark: 'github-dark' },
        defaultColor: false,
    });
    const label = lang === 'text' ? '' : `<span class="lang">${labels[lang] ?? lang}</span>`;
    const block = `<div class="code">${label}<button type="button" class="copy" aria-label="Copy code" title="Copy code"></button>${code}</div>`;
    return (env as Env).grouped ? `<div class="block">${block}</div>` : block;
};

md.renderer.rules.heading_close = (tokens, idx) => {
    const open = tokens[idx - 2];
    const id = open.attrGet('id');
    const anchor = id && open.tag !== 'h1' ? ` <a class="anchor" href="#${id}" aria-label="Link to this section"></a>` : '';
    return `${anchor}</${tokens[idx].tag}>\n`;
};

// Links between pages go by the files' names, as on GitHub: `./systems.md`,
// `../engine/networking.md#rooms`. Here they become the pages' paths. Links
// out of the site open in a new tab.
md.renderer.rules.link_open = (tokens, idx, options, env, self) => {
    const { file, isPage } = env as Env;
    const token = tokens[idx];
    const href = String(token.attrGet('href') ?? '');
    if (/^[a-z][a-z0-9+.-]*:|^\/\//i.test(href)) {
        token.attrSet('target', '_blank');
        token.attrSet('rel', 'noreferrer');
    } else if (!href.startsWith('#')) {
        const hash = href.indexOf('#');
        const target = hash < 0 ? href : href.slice(0, hash);
        let path = new URL(target, `https://site/${file}`).pathname.replace(/\.md$/, '').replace(/(^|\/)index$/, '$1');
        if (path !== '/' && path.endsWith('/') && isPage(path.slice(0, -1))) path = path.slice(0, -1);
        token.attrSet('href', path + (hash < 0 ? '' : href.slice(hash)));
    }
    return self.renderToken(tokens, idx, options);
};

// Wide tables scroll on their own, not the page.
md.renderer.rules.table_open = () => '<div class="table"><table>\n';
md.renderer.rules.table_close = () => '</table></div>\n';

function block(name: string, render: RendererRule) {
    md.use(container, name, { render });
}

const callouts: Record<string, string> = { tip: 'Tip', info: 'Info', warning: 'Warning', danger: 'Danger' };
for (const [kind, title] of Object.entries(callouts)) {
    block(kind, (tokens, idx) => {
        const token = tokens[idx];
        if (token.nesting !== 1) return '</div>\n';
        const custom = token.info.trim().slice(kind.length).trim();
        return `<div class="callout ${kind}"><p class="callout-title">${escape(custom || title)}</p>\n`;
    });
}

block('details', (tokens, idx) => {
    const token = tokens[idx];
    if (token.nesting !== 1) return '</details>\n';
    const title = token.info.trim().slice('details'.length).trim();
    return `<details class="callout details"><summary>${escape(title || 'Details')}</summary>\n`;
});

// Tabs over the fences inside, each named by its fence's `[label]`. Radio
// buttons pick the tab, so they work before the page's script loads.
block('code-group', (tokens, idx, _options, env) => {
    const group = env as Env;
    if (tokens[idx].nesting !== 1) {
        group.grouped = false;
        return '</div></div>\n';
    }
    const name = `group-${++group.groups}`;
    const labels: string[] = [];
    for (let i = idx + 1, depth = 0; i < tokens.length; i++) {
        const t = tokens[i];
        if (t.type === 'container_code-group_open') depth++;
        if (t.type === 'container_code-group_close' && depth-- === 0) break;
        if (t.type === 'fence') labels.push(/\[(.+)\]/.exec(t.info)?.[1] ?? language(t.info.trim().split(/\s+/)[0]));
    }
    group.grouped = true;
    const tabs = labels
        .map((label, i) => `<input type="radio" name="${name}" id="${name}-${i}"${i === 0 ? ' checked' : ''}><label for="${name}-${i}">${escape(label)}</label>`)
        .join('');
    return `<div class="code-group"><div class="tabs">${tabs}</div><div class="blocks">\n`;
});

/** Renders one page of the docs. */
export function render(source: string, file: string, isPage: (path: string) => boolean): Rendered {
    const env: Env = { file, isPage, grouped: false, groups: 0 };
    const tokens = md.parse(source, env);

    // Anchors on headings, the outline (h2 and h3) and the sections search
    // finds, split at headings down to h3.
    let title = '';
    const headings: Heading[] = [];
    const sections: Section[] = [{ id: '', titles: [], text: '' }];
    const trail: string[] = [];
    const used = new Map<string, number>();
    for (let i = 0; i < tokens.length; i++) {
        const token = tokens[i];
        if (token.type === 'heading_open') {
            const inline = tokens[i + 1];
            const text = plain(inline);
            const level = Number(token.tag.slice(1));
            const base = slugify(text);
            const count = used.get(base) ?? 0;
            used.set(base, count + 1);
            const id = count ? `${base}-${count}` : base;
            token.attrSet('id', id);
            if (level === 1 && !title) {
                title = text;
                sections[0].titles = [text];
                trail[0] = text;
            } else if (level <= 3) {
                trail.length = level - 1;
                trail[level - 1] = text;
                headings.push({ level, id, html: md.renderer.renderInline(inline.children ?? [], md.options, env).replace(/<\/?a\b[^>]*>/g, '') });
                sections.push({ id, titles: trail.filter(Boolean), text: '' });
            }
            i++;
        } else if (token.type === 'inline') {
            const section = sections[sections.length - 1];
            section.text += (section.text ? ' ' : '') + plain(token);
        }
    }

    return { title, html: md.renderer.render(tokens, md.options, env), headings, sections };
}
