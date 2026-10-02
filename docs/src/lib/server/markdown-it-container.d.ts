// markdown-it-container ships no types of its own, and DefinitelyTyped's are
// for markdown-it's older ones.
declare module 'markdown-it-container' {
    import type { MarkdownIt, RendererRule } from 'markdown-it';

    export default function container(md: MarkdownIt, name: string, options?: { marker?: string; validate?: (params: string) => boolean; render?: RendererRule }): void;
}
