// The docs site, published to GitHub Pages by .github/workflows/docs.yml.
// `npm run dev` in docs/ serves it locally.

import { defineConfig } from 'vitepress';
import grammar from '../../tools/purrlang-syntax/syntaxes/purrlang.tmLanguage.json' with { type: 'json' };

const repo = 'https://github.com/BlenMiner/PurrEngine';

export default defineConfig({
    title: 'PurrEngine',
    description: 'A networking-first game engine: deterministic simulation, rollback netcode, and PurrLang, which compiles to C.',
    base: '/PurrEngine/',
    cleanUrls: true,
    head: [['link', { rel: 'icon', href: '/PurrEngine/favicon.svg' }]],
    // The web demo, which the workflow builds with CMake and copies to public/demo/.
    ignoreDeadLinks: [/^\/demo\/$/],

    markdown: {
        // The same grammar as the editors, so code looks the same here.
        languages: [{ ...grammar, name: 'purr', aliases: ['purrlang'] } as any],
        config(md) {
            // The repo's Markdown fences PurrLang as csharp, which GitHub
            // highlights; the site has the real grammar.
            const fence = md.renderer.rules.fence!;
            md.renderer.rules.fence = (tokens, idx, ...rest) => {
                if (tokens[idx].info.trim() === 'csharp') tokens[idx].info = 'purr';
                return fence(tokens, idx, ...rest);
            };
            // Pages are Vue templates: `{{` in inline code would be one.
            const code = md.renderer.rules.code_inline!;
            md.renderer.rules.code_inline = (...args) => code(...args).replace('<code', '<code v-pre');
        },
    },

    themeConfig: {
        logo: '/favicon.svg',
        nav: [
            { text: 'Guide', link: '/guide/', activeMatch: '^/guide/' },
            { text: 'Language', link: '/language/basics', activeMatch: '^/language/' },
            { text: 'Engine', link: '/engine/determinism', activeMatch: '^/engine/' },
            { text: 'Spec', link: '/purrlang' },
            { text: 'Demo', link: '/guide/demo' },
        ],
        sidebar: [
            {
                text: 'Guide',
                items: [
                    { text: 'Introduction', link: '/guide/' },
                    { text: 'Install', link: '/guide/install' },
                    { text: 'Your first game', link: '/guide/first-game' },
                    { text: 'The purr command', link: '/guide/cli' },
                    { text: 'Editors', link: '/guide/editors' },
                    { text: 'Try the demo', link: '/guide/demo' },
                ],
            },
            {
                text: 'Language',
                items: [
                    { text: 'Basics', link: '/language/basics' },
                    { text: 'Components and entities', link: '/language/entities' },
                    { text: 'Systems', link: '/language/systems' },
                    { text: 'Structs and functions', link: '/language/functions' },
                    { text: 'Math', link: '/language/math' },
                    { text: 'Text and lists', link: '/language/text-and-lists' },
                    { text: 'Events', link: '/language/events' },
                    { text: 'Scenes', link: '/language/scenes' },
                    { text: 'Input', link: '/language/input' },
                    { text: 'Views and drawing', link: '/language/views' },
                    { text: 'GUI', link: '/language/gui' },
                    { text: 'Local state', link: '/language/local-state' },
                    { text: 'Multiplayer', link: '/language/multiplayer' },
                    { text: 'Namespaces and files', link: '/language/namespaces' },
                    { text: 'Calling C', link: '/language/c-functions' },
                ],
            },
            {
                text: 'Engine',
                items: [
                    { text: 'Determinism', link: '/engine/determinism' },
                    { text: 'Networking', link: '/engine/networking' },
                    { text: 'The schedule', link: '/engine/schedule' },
                    { text: 'C hosts', link: '/engine/c-hosts' },
                ],
            },
            {
                text: 'Reference',
                items: [{ text: 'Language spec', link: '/purrlang' }],
            },
        ],
        socialLinks: [{ icon: 'github', link: repo }],
        search: { provider: 'local' },
        editLink: { pattern: `${repo}/edit/dev/docs/:path`, text: 'Edit this page on GitHub' },
        outline: { level: [2, 3] },
    },
});
