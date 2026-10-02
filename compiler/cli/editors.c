#include "editors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"

typedef struct editor {
    const char *name;
    const char *command;    // Looked up on PATH
    const char *installed;  // Where its installer puts the command, in an install root (see install_roots)
    const char *flatpak;    // Its Flatpak's ID, on Linux
    const char *extensions; // Its extensions folder, in the home folder (Flatpaks' too)
} editor;

// The VS Code family: they all install extensions with `<command> --install-extension`.
static const editor editors[] = {
#if defined(_WIN32)
    // Batch files: VS Code's bin also has a `code` shell script, for Git Bash.
    {"VS Code", "code.cmd", "Microsoft VS Code/bin/code.cmd", NULL, ".vscode/extensions"},
    {"VS Code Insiders", "code-insiders.cmd", "Microsoft VS Code Insiders/bin/code-insiders.cmd", NULL,
     ".vscode-insiders/extensions"},
    {"Cursor", "cursor.cmd", "cursor/resources/app/bin/cursor.cmd", NULL, ".cursor/extensions"},
    {"VSCodium", "codium.cmd", "VSCodium/bin/codium.cmd", NULL, ".vscode-oss/extensions"},
    {"Windsurf", "windsurf.cmd", "Windsurf/bin/windsurf.cmd", NULL, ".windsurf/extensions"},
#elif defined(__APPLE__)
    // On macOS, the commands are only on PATH if the user asked for them.
    {"VS Code", "code", "Visual Studio Code.app/Contents/Resources/app/bin/code", NULL, ".vscode/extensions"},
    {"VS Code Insiders", "code-insiders", "Visual Studio Code - Insiders.app/Contents/Resources/app/bin/code-insiders",
     NULL, ".vscode-insiders/extensions"},
    {"Cursor", "cursor", "Cursor.app/Contents/Resources/app/bin/cursor", NULL, ".cursor/extensions"},
    {"VSCodium", "codium", "VSCodium.app/Contents/Resources/app/bin/codium", NULL, ".vscode-oss/extensions"},
    {"Windsurf", "windsurf", "Windsurf.app/Contents/Resources/app/bin/windsurf", NULL, ".windsurf/extensions"},
#else
    // Package managers put the commands on PATH; Flatpak doesn't.
    {"VS Code", "code", NULL, "com.visualstudio.code", ".vscode/extensions"},
    {"VS Code Insiders", "code-insiders", NULL, NULL, ".vscode-insiders/extensions"},
    {"Cursor", "cursor", NULL, NULL, ".cursor/extensions"},
    {"VSCodium", "codium", NULL, "com.vscodium.codium", ".vscode-oss/extensions"},
    {"Windsurf", "windsurf", NULL, NULL, ".windsurf/extensions"},
#endif
};

static const char *home_dir(void)
{
    const char *home = sys_env("USERPROFILE");
    return home ? home : sys_env("HOME");
}

// Where editors install themselves, the user's own first: %LOCALAPPDATA%\Programs
// and %ProgramFiles% on Windows, ~/Applications and /Applications on macOS.
// Each is malloc'd, or NULL when it's unknown; the list ends at `count`.
static int install_roots(char **roots)
{
    int count = 0;
#if defined(_WIN32)
    const char *local = sys_env("LOCALAPPDATA");
    const char *system = sys_env("ProgramFiles");
    roots[count++] = local ? path_join(local, "Programs") : NULL;
    roots[count++] = system ? path_join(system, "") : NULL;
#elif defined(__APPLE__)
    const char *home = home_dir();
    roots[count++] = home ? path_join(home, "Applications") : NULL;
    roots[count++] = path_join("/Applications", "");
#else
    (void)roots;
#endif
    return count;
}

// Flatpaks' commands: `flatpak run <id>`, as a program of their own, for the
// user's Flatpaks and the system's.
static char *find_flatpak(const char *id)
{
    const char *home = home_dir();
    char *user = home ? path_join(home, ".local/share/flatpak/exports/bin") : NULL;
    const char *const dirs[] = {user, "/var/lib/flatpak/exports/bin"};
    char *found = NULL;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0] && !found; i++) {
        if (!dirs[i]) continue;
        char *path = path_join(dirs[i], id);
        if (sys_exists(path)) found = path;
        else free(path);
    }
    free(user);
    return found;
}

// The editor's command, or NULL if it isn't installed.
static char *find_editor(const editor *e)
{
    char *found = sys_which(e->command);
    if (found) return found;
    if (e->installed) {
        char *roots[4];
        const int count = install_roots(roots);
        for (int i = 0; i < count; i++) {
            if (roots[i] && !found) {
                char *path = path_join(roots[i], e->installed);
                if (sys_exists(path)) found = path;
                else free(path);
            }
            free(roots[i]);
        }
    }
    if (!found && e->flatpak) found = find_flatpak(e->flatpak);
    return found;
}

// The extension's folder is tide-engine.tide-<version>; tide-engine.tide-syntax-<version>
// is the grammar alone (tools/tide-syntax), which doesn't count.
static void find_extension(void *user, const char *name, const bool is_dir)
{
    static const char prefix[] = "tide-engine.tide-";
    const size_t n = sizeof prefix - 1;
    if (is_dir && strncmp(name, prefix, n) == 0 && name[n] >= '0' && name[n] <= '9') *(bool *)user = true;
}

// Whether the editor has the extension already.
static bool has_extension(const editor *e)
{
    const char *home = home_dir();
    if (!home) return false;
    char *dir = path_join(home, e->extensions);
    bool found = false;
    sys_list(dir, find_extension, &found);
    free(dir);
    return found;
}

int tide_editors(const char *root, const bool only_updates)
{
    char *vsix = path_join(root, "editors/tide.vsix");
    if (!sys_exists(vsix)) {
        if (!only_updates) fprintf(stderr, "tide: there's no editor extension in %s; reinstall tide\n", root);
        free(vsix);
        return only_updates ? 0 : 1;
    }

    int found = 0;
    int added = 0;
    int failed = 0;
    for (size_t i = 0; i < sizeof editors / sizeof editors[0]; i++) {
        const editor *e = &editors[i];
        char *command = find_editor(e);
        if (!command) continue;
        found++;
        if (only_updates && !has_extension(e)) {
            free(command);
            continue;
        }
        const char *const argv[] = {command, "--install-extension", vsix, "--force", NULL};
        if (sys_run(argv, NULL, true) == 0) {
            printf(only_updates ? "Updated Tide in %s.\n" : "Added Tide to %s.\n", e->name);
            added++;
        } else {
            fprintf(stderr, "tide: couldn't add Tide to %s (%s --install-extension failed)\n", e->name, command);
            failed++;
        }
        free(command);
    }
    if (added > 0) printf("Windows of these editors that are open get it once reloaded: Developer: Reload Window.\n");
    if (found == 0 && !only_updates) {
        printf("tide found no editor to add Tide to (VS Code, Cursor, VSCodium or Windsurf).\n"
               "For JetBrains IDEs and others, see https://github.com/BlenMiner/tide-engine#editors\n");
    }
    free(vsix);
    return failed > 0 ? 1 : 0;
}
