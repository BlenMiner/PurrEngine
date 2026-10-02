// The site's navigation. Every page is in the sidebar, in reading order, and
// the build fails on a Markdown file that isn't (see server/content.ts). A
// page's path is its file's, without `.md`; `index.md` is its folder's.

export const repo = 'https://github.com/BlenMiner/tide-engine';

export interface Link {
    text: string;
    link: string;
}

export interface Group {
    text: string;
    items: Link[];
}

export const nav: (Link & { match: string })[] = [
    { text: 'Guide', link: '/guide', match: '/guide' },
    { text: 'Language', link: '/language/basics', match: '/language/' },
    { text: 'Engine', link: '/engine/determinism', match: '/engine/' },
    { text: 'Spec', link: '/spec', match: '/spec' },
    { text: 'Demo', link: '/guide/sand', match: '/guide/sand' },
];

export const sidebar: Group[] = [
    {
        text: 'Guide',
        items: [
            { text: 'Introduction', link: '/guide' },
            { text: 'Install', link: '/guide/install' },
            { text: 'Your first game', link: '/guide/first-game' },
            { text: 'The tide command', link: '/guide/cli' },
            { text: 'Editors', link: '/guide/editors' },
            { text: 'Falling sand', link: '/guide/sand' },
        ],
    },
    {
        text: 'Language',
        items: [
            { text: 'Basics', link: '/language/basics' },
            { text: 'Components and entities', link: '/language/entities' },
            { text: 'Systems', link: '/language/systems' },
            { text: 'Structs and functions', link: '/language/functions' },
            { text: 'Errors', link: '/language/errors' },
            { text: 'Math', link: '/language/math' },
            { text: 'Text and lists', link: '/language/text-and-lists' },
            { text: 'Grids', link: '/language/grids' },
            { text: 'Events', link: '/language/events' },
            { text: 'Async and tasks', link: '/language/tasks' },
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
        items: [{ text: 'Language spec', link: '/spec' }],
    },
];

/** Every page, in reading order. */
export const pages: Link[] = sidebar.flatMap((group) => group.items);

/** The paths of the pages, without their leading `/`, as routes see them. */
export const slugs = new Set(pages.map((page) => page.link.slice(1)));
