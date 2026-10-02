<script lang="ts">
    import '@fontsource-variable/inter';
    import '../app.css';
    import Header from '#lib/components/Header.svelte';
    import Search from '#lib/components/Search.svelte';
    import { watchTheme } from '#lib/theme.svelte.ts';

    let { children } = $props();

    let searching = $state(false);

    $effect(watchTheme);
</script>

<svelte:head>
    <meta name="theme-color" content="#1b1b1f" media="(prefers-color-scheme: dark)" />
    <meta name="theme-color" content="#ffffff" media="(prefers-color-scheme: light)" />
    <meta property="og:site_name" content="Tide" />
    <meta property="og:type" content="website" />
</svelte:head>

<a class="skip" href="#content">Skip to content</a>
<Header onsearch={() => (searching = true)} />
{@render children()}
<Search bind:open={searching} />

<style>
    .skip {
        position: fixed;
        top: 8px;
        left: 8px;
        z-index: 50;
        padding: 8px 16px;
        border-radius: 8px;
        background: var(--bg);
        color: var(--brand-1);
        font-weight: 600;
        box-shadow: var(--shadow-3);
        transform: translateY(-200%);
    }

    .skip:focus {
        transform: none;
    }
</style>
