<script lang="ts">
    import { goto } from '$app/navigation';
    import MiniSearch, { type SearchResult } from 'minisearch';
    import { tick, untrack } from 'svelte';

    let { open = $bindable(false) }: { open?: boolean } = $props();

    interface Entry {
        id: string;
        titles: string[];
        text: string;
    }

    interface Doc {
        id: string;
        title: string;
        trail: string;
        text: string;
    }

    let index: MiniSearch<Doc> | undefined = $state();
    let failed = $state(false);
    let query = $state('');
    let selected = $state(0);
    let input: HTMLInputElement | undefined = $state();
    let list: HTMLElement | undefined = $state();

    // The index loads the first time search opens, not with every page.
    async function load() {
        if (index || failed) return;
        try {
            const response = await fetch('/search.json');
            const entries: Entry[] = await response.json();
            const search = new MiniSearch<Doc>({
                fields: ['title', 'trail', 'text'],
                storeFields: ['title', 'trail', 'text'],
                searchOptions: { boost: { title: 4, trail: 2 }, prefix: true, fuzzy: 0.2, combineWith: 'AND' },
            });
            search.addAll(entries.map((e) => ({ id: e.id, title: e.titles.at(-1) ?? '', trail: e.titles.slice(0, -1).join(' › '), text: e.text })));
            index = search;
        } catch {
            failed = true;
        }
    }

    const results = $derived(index && query.trim() ? index.search(query.trim()).slice(0, 30) : []);

    $effect(() => {
        results;
        selected = 0;
    });

    $effect(() => {
        if (!open) return;
        untrack(() => {
            load();
            tick().then(() => input?.select());
        });
    });

    /** The part of a section's text around the first word searched for, with the matches marked. */
    function excerpt(result: SearchResult): { text: string; mark: boolean }[] {
        const text: string = result.text ?? '';
        const terms = result.terms.filter(Boolean).sort((a, b) => b.length - a.length);
        if (!terms.length || !text) return [{ text: text.slice(0, 140), mark: false }];
        const pattern = new RegExp(terms.map((t) => t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')).join('|'), 'gi');
        const first = text.search(pattern);
        const start = first < 0 ? 0 : Math.max(0, text.lastIndexOf(' ', Math.max(0, first - 40)) + 1);
        const slice = (start > 0 ? '…' : '') + text.slice(start, start + 160) + (start + 160 < text.length ? '…' : '');
        const parts: { text: string; mark: boolean }[] = [];
        let at = 0;
        for (const match of slice.matchAll(pattern)) {
            if (match.index > at) parts.push({ text: slice.slice(at, match.index), mark: false });
            parts.push({ text: match[0], mark: true });
            at = match.index + match[0].length;
        }
        if (at < slice.length) parts.push({ text: slice.slice(at), mark: false });
        return parts;
    }

    function go(result: SearchResult | undefined) {
        if (!result) return;
        open = false;
        query = '';
        goto(result.id);
    }

    function keydown(event: KeyboardEvent) {
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
            event.preventDefault();
            if (!results.length) return;
            selected = (selected + (event.key === 'ArrowDown' ? 1 : results.length - 1)) % results.length;
            tick().then(() => list?.querySelector('[aria-selected="true"]')?.scrollIntoView({ block: 'nearest' }));
        } else if (event.key === 'Enter') {
            event.preventDefault();
            go(results[selected]);
        } else if (event.key === 'Escape') {
            open = false;
        }
    }

    // Ctrl+K (Cmd+K) anywhere, or / outside a text field, opens it.
    function shortcut(event: KeyboardEvent) {
        const target = event.target as HTMLElement;
        const typing = target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName);
        if ((event.key === 'k' && (event.ctrlKey || event.metaKey)) || (event.key === '/' && !typing)) {
            event.preventDefault();
            open = !open || event.key === '/';
        }
    }
</script>

<svelte:window onkeydown={shortcut} />

{#if open}
    <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
    <div class="backdrop" onclick={() => (open = false)}></div>
    <div class="dialog" role="dialog" aria-modal="true" aria-label="Search the docs">
        <div class="field">
            <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="11" cy="11" r="7" /><path d="m20 20-3.5-3.5" /></svg>
            <input
                bind:this={input}
                bind:value={query}
                onkeydown={keydown}
                type="search"
                placeholder="Search the docs"
                aria-label="Search the docs"
                aria-controls="search-results"
                aria-activedescendant={results.length ? `result-${selected}` : undefined}
                autocomplete="off"
                spellcheck="false"
            />
            <button class="close" type="button" onclick={() => (open = false)} aria-label="Close search"><kbd>Esc</kbd></button>
        </div>

        <div class="results" id="search-results" role="listbox" bind:this={list}>
            {#if failed}
                <p class="note">Search couldn't load. Try again in a moment.</p>
            {:else if !index && query}
                <p class="note">Loading…</p>
            {:else if query.trim() && !results.length}
                <p class="note">Nothing found for “{query.trim()}”.</p>
            {:else}
                {#each results as result, i (result.id)}
                    <!-- svelte-ignore a11y_mouse_events_have_key_events -->
                    <a
                        id="result-{i}"
                        href={result.id}
                        role="option"
                        aria-selected={i === selected}
                        class:selected={i === selected}
                        onmouseover={() => (selected = i)}
                        onclick={(event) => {
                            event.preventDefault();
                            go(result);
                        }}
                    >
                        <span class="trail">{result.trail}</span>
                        <span class="title">{result.title}</span>
                        <span class="excerpt">
                            {#each excerpt(result) as part, j (j)}{#if part.mark}<mark>{part.text}</mark>{:else}{part.text}{/if}{/each}
                        </span>
                    </a>
                {/each}
            {/if}
        </div>

        <div class="footer">
            <span><kbd>↑</kbd> <kbd>↓</kbd> to move</span>
            <span><kbd>Enter</kbd> to go</span>
            <span><kbd>Esc</kbd> to close</span>
        </div>
    </div>
{/if}

<style>
    .backdrop {
        position: fixed;
        inset: 0;
        z-index: 40;
        background: var(--backdrop);
    }

    .dialog {
        position: fixed;
        top: 64px;
        left: 50%;
        z-index: 41;
        display: flex;
        flex-direction: column;
        width: min(640px, calc(100vw - 32px));
        max-height: calc(100dvh - 128px);
        transform: translateX(-50%);
        border-radius: 12px;
        background: var(--bg);
        box-shadow: var(--shadow-3);
        overflow: hidden;
    }

    .field {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 0 16px;
        border-bottom: 1px solid var(--divider);
    }

    .field svg {
        flex: none;
        width: 20px;
        height: 20px;
        fill: none;
        stroke: var(--text-3);
        stroke-width: 2;
        stroke-linecap: round;
    }

    input {
        flex: 1;
        min-width: 0;
        height: 56px;
        border: 0;
        outline: none;
        background: none;
        color: var(--text-1);
        font: inherit;
        font-size: 16px;
    }

    input::-webkit-search-cancel-button {
        display: none;
    }

    .results {
        flex: 1;
        overflow-y: auto;
        padding: 8px;
        overscroll-behavior: contain;
    }

    .results:empty {
        display: none;
    }

    .note {
        margin: 16px 8px;
        color: var(--text-2);
        font-size: 14px;
    }

    a {
        display: grid;
        gap: 2px;
        padding: 10px 12px;
        border-radius: 8px;
        border: 1px solid transparent;
    }

    a.selected {
        border-color: var(--brand-1);
        background: var(--brand-soft);
    }

    .trail {
        color: var(--text-3);
        font-size: 12px;
        font-weight: 500;
    }

    .trail:empty {
        display: none;
    }

    .title {
        color: var(--text-1);
        font-size: 15px;
        font-weight: 600;
    }

    .excerpt {
        color: var(--text-2);
        font-size: 13px;
        line-height: 1.5;
        display: -webkit-box;
        -webkit-line-clamp: 2;
        line-clamp: 2;
        -webkit-box-orient: vertical;
        overflow: hidden;
    }

    mark {
        background: none;
        color: var(--brand-1);
        font-weight: 600;
    }

    .footer {
        display: flex;
        gap: 16px;
        padding: 10px 16px;
        border-top: 1px solid var(--divider);
        color: var(--text-3);
        font-size: 12px;
    }

    kbd {
        padding: 1px 6px;
        border: 1px solid var(--divider);
        border-radius: 4px;
        background: var(--bg-alt);
        color: var(--text-2);
        font-family: var(--font);
        font-size: 11px;
        font-weight: 600;
    }

    @media (max-width: 639px) {
        .dialog {
            top: 0;
            width: 100vw;
            max-height: 100dvh;
            height: 100dvh;
            border-radius: 0;
        }

        .footer {
            display: none;
        }
    }
</style>
