<script lang="ts">
    let { data } = $props();

    // The copy buttons on code blocks.
    function copyButtons(article: HTMLElement) {
        const click = (event: MouseEvent) => {
            const button = (event.target as HTMLElement).closest('button.copy');
            const code = button?.parentElement?.querySelector('pre code');
            if (!button || !code) return;
            navigator.clipboard.writeText(code.textContent ?? '').then(() => {
                button.classList.add('copied');
                button.setAttribute('aria-label', 'Copied');
                setTimeout(() => {
                    button.classList.remove('copied');
                    button.setAttribute('aria-label', 'Copy code');
                }, 2000);
            });
        };
        article.addEventListener('click', click);
        return () => article.removeEventListener('click', click);
    }
</script>

<svelte:head>
    <title>{data.title} | Tide</title>
    <meta name="description" content={data.description} />
    <meta property="og:title" content={data.title} />
    <meta property="og:description" content={data.description} />
    <link rel="canonical" href="https://tide-engine.dev{data.path}" />
</svelte:head>

<article class="prose" {@attach copyButtons}>
    {@html data.html}
</article>

<footer>
    <a class="edit" href={data.edit} target="_blank" rel="noreferrer">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4Z" /></svg>
        Edit this page on GitHub
    </a>

    {#if data.prev || data.next}
        <nav class="pager" aria-label="Pages">
            {#if data.prev}
                <a class="prev" href={data.prev.link}>
                    <span>Previous page</span>
                    {data.prev.text}
                </a>
            {/if}
            {#if data.next}
                <a class="next" href={data.next.link}>
                    <span>Next page</span>
                    {data.next.text}
                </a>
            {/if}
        </nav>
    {/if}
</footer>

<style>
    footer {
        margin-top: 64px;
    }

    .edit {
        display: inline-flex;
        align-items: center;
        gap: 8px;
        color: var(--brand-1);
        font-size: 14px;
        font-weight: 500;
        transition: color 0.2s;
    }

    .edit:hover {
        color: var(--brand-2);
    }

    .edit svg {
        width: 16px;
        height: 16px;
        fill: none;
        stroke: currentColor;
        stroke-width: 2;
        stroke-linecap: round;
        stroke-linejoin: round;
    }

    .pager {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 16px;
        margin-top: 24px;
        padding-top: 24px;
        border-top: 1px solid var(--divider);
    }

    .pager a {
        display: flex;
        flex-direction: column;
        gap: 2px;
        padding: 12px 16px;
        border: 1px solid var(--divider);
        border-radius: 8px;
        color: var(--brand-1);
        font-size: 14px;
        font-weight: 500;
        transition: border-color 0.2s;
    }

    .pager a:hover {
        border-color: var(--brand-1);
    }

    .pager span {
        color: var(--text-2);
        font-size: 12px;
    }

    .next {
        grid-column: 2;
        text-align: right;
    }

    @media (max-width: 639px) {
        .pager {
            grid-template-columns: 1fr;
        }

        .next {
            grid-column: 1;
        }
    }
</style>
