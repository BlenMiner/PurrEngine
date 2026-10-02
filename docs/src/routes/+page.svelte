<script lang="ts">
    let { data } = $props();

    const external = (link: string) => /^https?:/.test(link);
    const description = $derived(`${data.hero.text}. ${data.hero.tagline}`);
</script>

<svelte:head>
    <title>{data.hero.name}: {data.hero.text.toLowerCase()}</title>
    <meta name="description" content={description} />
    <meta property="og:title" content="{data.hero.name}: {data.hero.text.toLowerCase()}" />
    <meta property="og:description" content={description} />
    <link rel="canonical" href="https://tide-engine.dev/" />
</svelte:head>

<main id="content">
    <section class="hero">
        <div class="words">
            <h1>
                <span class="name">{data.hero.name}</span>
                <span class="text">{data.hero.text}</span>
            </h1>
            <p class="tagline">{data.hero.tagline}</p>
            <div class="actions">
                {#each data.hero.actions as action (action.link)}
                    <a class="button {action.theme}" href={action.link} target={external(action.link) ? '_blank' : undefined} rel={external(action.link) ? 'noreferrer' : undefined}>{action.text}</a>
                {/each}
            </div>
        </div>
        <div class="image" aria-hidden="true">
            <div class="glow"></div>
            <img src="/favicon.svg" alt="" width="320" height="320" />
        </div>
    </section>

    <section class="features" aria-label="Features">
        {#each data.features as feature (feature.title)}
            <div class="feature">
                <h2>{feature.title}</h2>
                <p>{feature.details}</p>
            </div>
        {/each}
    </section>

    <section class="prose more">
        {@html data.html}
    </section>
</main>

<style>
    main {
        max-width: 1152px;
        margin: 0 auto;
        padding: 0 48px 128px;
    }

    .hero {
        display: flex;
        align-items: center;
        gap: 48px;
        padding: 96px 0 64px;
    }

    .words {
        flex: 1;
        min-width: 0;
    }

    h1 {
        margin: 0;
        font-size: 56px;
        font-weight: 700;
        line-height: 1.12;
        letter-spacing: -0.03em;
    }

    h1 span {
        display: block;
    }

    .name {
        width: fit-content;
        background: linear-gradient(120deg, #f59e0b 30%, #e0457b);
        -webkit-background-clip: text;
        background-clip: text;
        color: transparent;
    }

    .tagline {
        max-width: 576px;
        margin: 16px 0 0;
        color: var(--text-2);
        font-size: 22px;
        font-weight: 500;
        line-height: 1.5;
    }

    .actions {
        display: flex;
        flex-wrap: wrap;
        gap: 12px;
        margin-top: 32px;
    }

    .button {
        display: inline-block;
        padding: 0 20px;
        border: 1px solid transparent;
        border-radius: 20px;
        font-size: 14px;
        font-weight: 600;
        line-height: 38px;
        transition: background-color 0.2s, border-color 0.2s, color 0.2s;
    }

    .button.brand {
        background: var(--brand-3);
        color: #ffffff;
    }

    .button.brand:hover {
        background: var(--brand-2);
    }

    .button.alt {
        background: var(--bg-alt);
        border-color: var(--divider);
        color: var(--text-1);
    }

    .button.alt:hover {
        border-color: var(--border);
    }

    .image {
        position: relative;
        flex: none;
        display: grid;
        place-items: center;
        width: 360px;
        height: 360px;
    }

    .image img {
        position: relative;
        width: 280px;
        height: 280px;
    }

    .glow {
        position: absolute;
        inset: 15%;
        border-radius: 50%;
        background: linear-gradient(-45deg, #f59e0b 50%, #e0457b 50%);
        filter: blur(72px);
        opacity: 0.35;
    }

    .features {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 1fr));
        gap: 16px;
    }

    .feature {
        padding: 24px;
        border: 1px solid var(--bg-soft);
        border-radius: 12px;
        background: var(--bg-soft);
    }

    .feature h2 {
        margin: 0;
        font-size: 16px;
        font-weight: 600;
        line-height: 24px;
    }

    .feature p {
        margin: 8px 0 0;
        color: var(--text-2);
        font-size: 14px;
        line-height: 24px;
    }

    .more {
        max-width: 760px;
        margin: 32px auto 0;
    }

    @media (max-width: 959px) {
        .hero {
            flex-direction: column-reverse;
            align-items: flex-start;
            gap: 8px;
            padding: 48px 0;
        }

        .image {
            width: 200px;
            height: 200px;
        }

        .image img {
            width: 160px;
            height: 160px;
        }

        .features {
            grid-template-columns: repeat(2, minmax(0, 1fr));
        }
    }

    @media (max-width: 639px) {
        main {
            padding: 0 24px 96px;
        }

        h1 {
            font-size: 40px;
        }

        .tagline {
            font-size: 18px;
        }

        .features {
            grid-template-columns: 1fr;
        }
    }
</style>
