<script lang="ts">
    import { page } from '$app/state';
</script>

<svelte:head>
    <title>{page.status === 404 ? 'Page not found' : 'Something went wrong'} | Tide</title>
</svelte:head>

<main id="content">
    <p class="code">{page.status}</p>
    <h1>{page.status === 404 ? 'There’s no page here' : 'Something went wrong'}</h1>
    <p>
        {#if page.status === 404}
            It may have moved. The <a href="/guide">guide</a> starts from the beginning, or search for what you were after.
        {:else}
            {page.error?.message}
        {/if}
    </p>
    <a class="home" href="/">Take me home</a>
</main>

<style>
    main {
        max-width: 560px;
        margin: 0 auto;
        padding: 96px 24px;
        text-align: center;
    }

    .code {
        margin: 0;
        font-size: 64px;
        font-weight: 700;
        background: linear-gradient(120deg, #f59e0b 30%, #e0457b);
        -webkit-background-clip: text;
        background-clip: text;
        color: transparent;
    }

    h1 {
        margin: 8px 0 16px;
        font-size: 24px;
    }

    p {
        color: var(--text-2);
    }

    p a {
        color: var(--brand-1);
        font-weight: 500;
    }

    .home {
        display: inline-block;
        margin-top: 24px;
        padding: 0 20px;
        border-radius: 20px;
        border: 1px solid var(--brand-1);
        color: var(--brand-1);
        font-size: 14px;
        font-weight: 600;
        line-height: 38px;
    }
</style>
