<script lang="ts">
    import type { Heading } from '#lib/server/markdown.ts';

    let { headings, spy = false, onpick }: { headings: Heading[]; spy?: boolean; onpick?: () => void } = $props();

    let active = $state('');

    // The section being read: the last heading that scrolled past the top,
    // or the last one of all once the page can't scroll further.
    $effect(() => {
        if (!spy) return;
        const elements = headings.map((h) => document.getElementById(h.id)).filter((e) => e !== null);
        let frame = 0;
        const update = () => {
            frame = 0;
            const top = parseFloat(getComputedStyle(document.documentElement).scrollPaddingTop) || 0;
            const bottom = window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 2;
            let current = '';
            for (const element of elements) {
                if (element.getBoundingClientRect().top <= top + 8) current = element.id;
                else break;
            }
            active = bottom && elements.length ? elements[elements.length - 1].id : current;
        };
        const schedule = () => (frame ||= requestAnimationFrame(update));
        update();
        addEventListener('scroll', schedule, { passive: true });
        addEventListener('resize', schedule, { passive: true });
        return () => {
            removeEventListener('scroll', schedule);
            removeEventListener('resize', schedule);
            cancelAnimationFrame(frame);
        };
    });
</script>

<ul>
    {#each headings as heading (heading.id)}
        <li class:sub={heading.level === 3}>
            <a href="#{heading.id}" class:active={spy && active === heading.id} onclick={onpick}>{@html heading.html}</a>
        </li>
    {/each}
</ul>

<style>
    ul {
        margin: 0;
        padding: 0;
        list-style: none;
    }

    li.sub {
        padding-left: 12px;
    }

    a {
        display: block;
        padding: 3px 0;
        overflow: hidden;
        color: var(--text-2);
        font-size: 13px;
        font-weight: 500;
        line-height: 1.5;
        text-overflow: ellipsis;
        transition: color 0.2s;
    }

    a:hover,
    a.active {
        color: var(--brand-1);
    }

    a :global(code) {
        font-family: var(--mono);
        font-size: 0.92em;
    }
</style>
