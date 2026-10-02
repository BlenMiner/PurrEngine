<script lang="ts">
    import { page } from '$app/state';
    import Outline from '#lib/components/Outline.svelte';
    import { sidebar } from '#lib/nav.ts';

    let { children } = $props();

    let menu = $state(false);
    let outline = $state(false);

    const headings = $derived(page.data.headings ?? []);

    $effect(() => {
        page.url.pathname;
        menu = false;
        outline = false;
    });
</script>

<svelte:window onkeydown={(event) => event.key === 'Escape' && (menu = outline = false)} />

<div class="local">
    <button type="button" class="menu" onclick={() => (menu = true)} aria-expanded={menu} aria-controls="sidebar">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 7h16M4 12h10M4 17h16" /></svg>
        Menu
    </button>
    {#if headings.length}
        <div class="on-page">
            <button type="button" onclick={() => (outline = !outline)} aria-expanded={outline}>
                On this page
                <svg viewBox="0 0 24 24" aria-hidden="true" class:flip={outline}><path d="m9 6 6 6-6 6" /></svg>
            </button>
            {#if outline}
                <div class="dropdown">
                    <button type="button" class="top" onclick={() => { scrollTo({ top: 0 }); outline = false; }}>Back to top</button>
                    <Outline {headings} onpick={() => (outline = false)} />
                </div>
            {/if}
        </div>
    {/if}
</div>

{#if menu}
    <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
    <div class="backdrop" onclick={() => (menu = false)}></div>
{/if}

<aside id="sidebar" class="sidebar" class:open={menu}>
    <nav aria-label="Docs">
        {#each sidebar as group (group.text)}
            <section>
                <h2>{group.text}</h2>
                <ul>
                    {#each group.items as item (item.link)}
                        <li>
                            <a href={item.link} class:active={page.url.pathname === item.link} aria-current={page.url.pathname === item.link ? 'page' : undefined}>{item.text}</a>
                        </li>
                    {/each}
                </ul>
            </section>
        {/each}
    </nav>
</aside>

<div class="main">
    <div class="content">
        <main id="content">
            {@render children()}
        </main>
        {#if headings.length}
            <aside class="outline" aria-label="On this page">
                <p>On this page</p>
                <Outline {headings} spy />
            </aside>
        {/if}
    </div>
</div>

<style>
    /* The sidebar's column reaches the left edge of wide screens. */
    .sidebar {
        position: fixed;
        top: var(--header-h);
        bottom: 0;
        left: 0;
        z-index: 20;
        width: calc(var(--sidebar-w) + max(0px, (100% - var(--max-w)) / 2));
        padding: 32px 24px 96px max(32px, calc((100% - var(--max-w)) / 2 + 32px));
        overflow-y: auto;
        overscroll-behavior: contain;
        background: var(--bg-alt);
        border-right: 1px solid var(--divider);
        scrollbar-width: thin;
    }

    section + section {
        margin-top: 8px;
        padding-top: 8px;
        border-top: 1px solid var(--divider);
    }

    h2 {
        margin: 0;
        padding: 6px 0;
        font-size: 14px;
        font-weight: 700;
        line-height: 24px;
    }

    ul {
        margin: 0 0 8px;
        padding: 0;
        list-style: none;
    }

    li a {
        display: block;
        padding: 4px 0;
        color: var(--text-2);
        font-size: 14px;
        font-weight: 500;
        line-height: 24px;
        transition: color 0.2s;
    }

    li a:hover {
        color: var(--text-1);
    }

    li a.active {
        color: var(--brand-1);
    }

    .main {
        padding-left: calc(var(--sidebar-w) + max(0px, (100% - var(--max-w)) / 2));
    }

    .content {
        display: flex;
        justify-content: center;
        gap: 64px;
        max-width: calc(var(--max-w) - var(--sidebar-w));
        padding: 48px 48px 128px 64px;
    }

    main {
        flex: 1;
        min-width: 0;
        max-width: 760px;
    }

    .outline {
        position: sticky;
        top: calc(var(--header-h) + 48px);
        flex: none;
        align-self: flex-start;
        width: 224px;
        max-height: calc(100vh - var(--header-h) - 96px);
        overflow-y: auto;
        scrollbar-width: thin;
    }

    .outline > p {
        margin: 0 0 4px;
        font-size: 14px;
        font-weight: 600;
        line-height: 32px;
    }

    .local {
        display: none;
    }

    .backdrop {
        display: none;
    }

    @media (max-width: 1279px) {
        .outline {
            display: none;
        }

        .local {
            position: sticky;
            top: var(--header-h);
            z-index: 25;
            display: flex;
            align-items: center;
            justify-content: space-between;
            height: var(--local-h);
            padding: 0 32px;
            margin-left: calc(var(--sidebar-w));
            background: color-mix(in srgb, var(--bg) 92%, transparent);
            backdrop-filter: saturate(180%) blur(12px);
            -webkit-backdrop-filter: saturate(180%) blur(12px);
            border-bottom: 1px solid var(--divider);
            font-size: 13px;
            font-weight: 500;
            color: var(--text-2);
        }

        .local .menu {
            visibility: hidden;
        }

        .local button {
            display: flex;
            align-items: center;
            gap: 6px;
            height: var(--local-h);
            transition: color 0.2s;
        }

        .local button:hover {
            color: var(--text-1);
        }

        .on-page {
            position: relative;
            margin-left: auto;
        }

        .local svg {
            width: 16px;
            height: 16px;
            fill: none;
            stroke: currentColor;
            stroke-width: 2;
            stroke-linecap: round;
            stroke-linejoin: round;
            transition: transform 0.2s;
        }

        .local svg.flip {
            transform: rotate(90deg);
        }

        .dropdown {
            position: absolute;
            top: calc(var(--local-h) - 4px);
            right: 0;
            width: min(320px, calc(100vw - 48px));
            max-height: 60vh;
            padding: 12px 16px;
            overflow-y: auto;
            border: 1px solid var(--divider);
            border-radius: 8px;
            background: var(--bg-elv);
            box-shadow: var(--shadow-3);
        }

        .local .dropdown .top {
            display: block;
            width: 100%;
            height: auto;
            text-align: left;
            margin-bottom: 8px;
            padding-bottom: 8px;
            border-bottom: 1px solid var(--divider);
            color: var(--brand-1);
            font-size: 13px;
            font-weight: 500;
        }

        .content {
            padding: 32px 48px 96px;
        }
    }

    @media (max-width: 959px) {
        .local {
            margin-left: 0;
            padding: 0 24px;
        }

        .local .menu {
            visibility: visible;
        }

        .sidebar {
            top: 0;
            z-index: 45;
            width: min(320px, calc(100vw - 64px));
            padding: 24px 24px 96px 32px;
            border-right: 0;
            transform: translateX(-100%);
            visibility: hidden;
            transition: transform 0.25s ease, visibility 0.25s;
        }

        .sidebar.open {
            transform: none;
            visibility: visible;
            box-shadow: var(--shadow-3);
        }

        .backdrop {
            display: block;
            position: fixed;
            inset: 0;
            z-index: 44;
            background: var(--backdrop);
        }

        .main {
            padding-left: 0;
        }

        .content {
            padding: 24px 24px 96px;
        }
    }
</style>
