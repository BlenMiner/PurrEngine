<script lang="ts">
    import { page } from '$app/state';
    import { nav, repo } from '#lib/nav.ts';
    import { theme, toggleTheme } from '#lib/theme.svelte.ts';

    let { onsearch }: { onsearch: () => void } = $props();

    let menu = $state(false);

    // A link lights up on its page and those under it, but "Demo" is a page
    // of the guide: only it lights up there.
    const under = (path: string, match: string) => path === match || path.startsWith(match.endsWith('/') ? match : `${match}/`);
    const current = $derived(nav.find((item) => item.match === page.url.pathname) ?? nav.find((item) => under(page.url.pathname, item.match)));

    $effect(() => {
        page.url.pathname;
        menu = false;
    });
</script>

<header class="header" class:open={menu}>
    <div class="bar">
        <a class="logo" href="/" aria-label="Tide, home">
            <img src="/favicon.svg" alt="" width="28" height="28" />
            <span>Tide</span>
        </a>

        <nav class="links" aria-label="Main">
            {#each nav as item (item.link)}
                <a href={item.link} class:active={current === item} aria-current={current === item ? 'page' : undefined}>{item.text}</a>
            {/each}
        </nav>

        <button class="search" type="button" onclick={onsearch} aria-label="Search">
            <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="11" cy="11" r="7" /><path d="m20 20-3.5-3.5" /></svg>
            <span class="label">Search</span>
            <kbd>Ctrl K</kbd>
        </button>

        <div class="tools">
            <button class="icon" type="button" onclick={toggleTheme} aria-label={theme.dark ? 'Switch to light theme' : 'Switch to dark theme'} title={theme.dark ? 'Light theme' : 'Dark theme'}>
                {#if theme.dark}
                    <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="4" /><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4" /></svg>
                {:else}
                    <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20.5 14.1A8.5 8.5 0 0 1 9.9 3.5a8.5 8.5 0 1 0 10.6 10.6Z" /></svg>
                {/if}
            </button>
            <a class="icon" href={repo} target="_blank" rel="noreferrer" aria-label="Tide on GitHub" title="GitHub">
                <svg viewBox="0 0 24 24" aria-hidden="true" class="filled"
                    ><path
                        d="M12 .3a12 12 0 0 0-3.8 23.4c.6.1.8-.3.8-.6v-2c-3.3.7-4-1.6-4-1.6-.6-1.4-1.4-1.8-1.4-1.8-1-.7.1-.7.1-.7 1.2 0 1.9 1.2 1.9 1.2 1 1.8 2.8 1.3 3.5 1 0-.8.4-1.3.7-1.6-2.7-.3-5.5-1.3-5.5-6 0-1.2.5-2.3 1.3-3.1-.2-.4-.6-1.6 0-3.2 0 0 1-.3 3.4 1.2a11.5 11.5 0 0 1 6 0C17.3 4.6 18.3 5 18.3 5c.6 1.6.2 2.8.1 3.2.8.8 1.3 1.9 1.3 3.2 0 4.6-2.8 5.6-5.5 5.9.5.4.9 1.1.9 2.2v3.3c0 .3.2.7.8.6A12 12 0 0 0 12 .3"
                    /></svg
                >
            </a>
        </div>

        <button class="icon burger" type="button" onclick={() => (menu = !menu)} aria-expanded={menu} aria-controls="mobile-nav" aria-label="Menu">
            <svg viewBox="0 0 24 24" aria-hidden="true">
                {#if menu}<path d="M6 6l12 12M18 6 6 18" />{:else}<path d="M4 7h16M4 12h16M4 17h16" />{/if}
            </svg>
        </button>
    </div>

    {#if menu}
        <nav id="mobile-nav" class="mobile" aria-label="Main">
            {#each nav as item (item.link)}
                <a href={item.link} class:active={current === item}>{item.text}</a>
            {/each}
            <div class="row">
                <button type="button" onclick={toggleTheme}>{theme.dark ? 'Light theme' : 'Dark theme'}</button>
                <a href={repo} target="_blank" rel="noreferrer">GitHub</a>
            </div>
        </nav>
    {/if}
</header>

<style>
    .header {
        position: sticky;
        top: 0;
        z-index: 30;
        height: var(--header-h);
        background: color-mix(in srgb, var(--bg) 88%, transparent);
        backdrop-filter: saturate(180%) blur(12px);
        -webkit-backdrop-filter: saturate(180%) blur(12px);
        border-bottom: 1px solid var(--divider);
    }

    .bar {
        display: flex;
        align-items: center;
        gap: 8px;
        max-width: var(--max-w);
        height: 100%;
        margin: 0 auto;
        padding: 0 32px;
    }

    .logo {
        display: flex;
        align-items: center;
        gap: 10px;
        margin-right: auto;
        font-size: 18px;
        font-weight: 650;
        letter-spacing: -0.01em;
    }

    .links {
        display: flex;
        margin-right: 8px;
    }

    .links a {
        padding: 0 12px;
        line-height: var(--header-h);
        color: var(--text-1);
        font-size: 14px;
        font-weight: 500;
        transition: color 0.2s;
    }

    .links a:hover,
    .links a.active {
        color: var(--brand-1);
    }

    .search {
        display: flex;
        align-items: center;
        gap: 8px;
        height: 40px;
        padding: 0 10px 0 12px;
        border: 1px solid transparent;
        border-radius: 8px;
        background: var(--bg-alt);
        color: var(--text-2);
        font-size: 13px;
        font-weight: 500;
        transition: border-color 0.2s;
    }

    .search:hover {
        border-color: var(--brand-1);
    }

    .search svg {
        width: 16px;
        height: 16px;
    }

    .search .label {
        margin-right: 24px;
    }

    kbd {
        padding: 1px 6px;
        border: 1px solid var(--divider);
        border-radius: 4px;
        background: var(--bg);
        color: var(--text-3);
        font-family: var(--font);
        font-size: 11px;
        font-weight: 600;
        line-height: 18px;
    }

    .tools {
        display: flex;
        align-items: center;
        margin-left: 8px;
        padding-left: 8px;
        border-left: 1px solid var(--divider);
    }

    .icon {
        display: grid;
        place-items: center;
        width: 36px;
        height: 36px;
        border-radius: 8px;
        color: var(--text-2);
        transition: color 0.2s;
    }

    .icon:hover {
        color: var(--text-1);
    }

    svg {
        width: 20px;
        height: 20px;
        fill: none;
        stroke: currentColor;
        stroke-width: 2;
        stroke-linecap: round;
        stroke-linejoin: round;
    }

    svg.filled {
        fill: currentColor;
        stroke: none;
    }

    .burger {
        display: none;
    }

    /* Absolute, not fixed: the header's backdrop filter makes it the box
       that fixed children are placed in. */
    .mobile {
        position: absolute;
        top: 100%;
        left: 0;
        right: 0;
        height: calc(100dvh - var(--header-h));
        padding: 16px 32px 32px;
        overflow-y: auto;
        background: var(--bg);
    }

    .mobile > a {
        display: block;
        padding: 12px 0;
        border-bottom: 1px solid var(--divider);
        font-size: 15px;
        font-weight: 500;
    }

    .mobile > a.active {
        color: var(--brand-1);
    }

    .mobile .row {
        display: flex;
        gap: 24px;
        padding: 16px 0;
        color: var(--text-2);
        font-size: 14px;
        font-weight: 500;
    }

    @media (max-width: 959px) {
        .links {
            display: none;
        }

        .tools {
            display: none;
        }

        .burger {
            display: grid;
        }

        .bar {
            padding: 0 16px 0 24px;
        }

        .search .label,
        .search kbd {
            display: none;
        }

        .search {
            width: 36px;
            height: 36px;
            padding: 0;
            justify-content: center;
            background: none;
        }

        .search svg {
            width: 20px;
            height: 20px;
        }

        .search:hover {
            border-color: transparent;
            color: var(--text-1);
        }
    }
</style>
