// Light or dark. app.html sets the class before the page shows; this keeps it
// and the toggle in step, and follows the system until the visitor picks.

export const theme = $state({ dark: false });

export function watchTheme(): () => void {
    theme.dark = document.documentElement.classList.contains('dark');
    const system = matchMedia('(prefers-color-scheme: dark)');
    const follow = () => {
        let saved = null;
        try {
            saved = localStorage.getItem('theme');
        } catch {}
        if (saved === null) apply(system.matches);
    };
    system.addEventListener('change', follow);
    return () => system.removeEventListener('change', follow);
}

export function toggleTheme(): void {
    apply(!theme.dark);
    try {
        localStorage.setItem('theme', theme.dark ? 'dark' : 'light');
    } catch {}
}

function apply(dark: boolean): void {
    theme.dark = dark;
    document.documentElement.classList.toggle('dark', dark);
}
