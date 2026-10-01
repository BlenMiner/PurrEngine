#include "editors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"

typedef struct editor {
    const char *name;
    const char *command;    // Looked up on PATH
    const char *installed;  // Where its installer puts the command, in case it isn't on PATH (see installed_root)
    const char *extensions; // Its extensions folder, in the home folder
} editor;

// The VS Code family: they all install extensions with `<command> --install-extension`.
static const editor editors[] = {
#if defined(_WIN32)
    // Batch files: VS Code's bin also has a `code` shell script, for Git Bash.
    {"VS Code", "code.cmd", "Programs/Microsoft VS Code/bin/code.cmd", ".vscode/extensions"},
    {"VS Code Insiders", "code-insiders.cmd", "Programs/Microsoft VS Code Insiders/bin/code-insiders.cmd",
     ".vscode-insiders/extensions"},
    {"Cursor", "cursor.cmd", "Programs/cursor/resources/app/bin/cursor.cmd", ".cursor/extensions"},
    {"VSCodium", "codium.cmd", "Programs/VSCodium/bin/codium.cmd", ".vscode-oss/extensions"},
    {"Windsurf", "windsurf.cmd", "Programs/Windsurf/bin/windsurf.cmd", ".windsurf/extensions"},
#elif defined(__APPLE__)
    // On macOS, the commands are only on PATH if the user asked for them.
    {"VS Code", "code", "Visual Studio Code.app/Contents/Resources/app/bin/code", ".vscode/extensions"},
    {"VS Code Insiders", "code-insiders", "Visual Studio Code - Insiders.app/Contents/Resources/app/bin/code-insiders",
     ".vscode-insiders/extensions"},
    {"Cursor", "cursor", "Cursor.app/Contents/Resources/app/bin/cursor", ".cursor/extensions"},
    {"VSCodium", "codium", "VSCodium.app/Contents/Resources/app/bin/codium", ".vscode-oss/extensions"},
    {"Windsurf", "windsurf", "Windsurf.app/Contents/Resources/app/bin/windsurf", ".windsurf/extensions"},
#else
    {"VS Code", "code", NULL, ".vscode/extensions"},
    {"VS Code Insiders", "code-insiders", NULL, ".vscode-insiders/extensions"},
    {"Cursor", "cursor", NULL, ".cursor/extensions"},
    {"VSCodium", "codium", NULL, ".vscode-oss/extensions"},
    {"Windsurf", "windsurf", NULL, ".windsurf/extensions"},
#endif
};

// Where editors install themselves: %LOCALAPPDATA% on Windows, /Applications
// on macOS. NULL on Linux, where package managers put them on PATH.
static const char *installed_root(void)
{
#if defined(_WIN32)
    return sys_env("LOCALAPPDATA");
#elif defined(__APPLE__)
    return "/Applications";
#else
    return NULL;
#endif
}

// The editor's command, or NULL if it isn't installed.
static char *find_editor(const editor *e)
{
    char *found = sys_which(e->command);
    const char *root = installed_root();
    if (found || !e->installed || !root) return found;
    char *path = path_join(root, e->installed);
    if (sys_exists(path)) return path;
    free(path);
    return NULL;
}

static void find_extension(void *user, const char *name, const bool is_dir)
{
    if (is_dir && strncmp(name, "tide-engine.tide-", 17) == 0) *(bool *)user = true;
}

// Whether the editor has the extension already.
static bool has_extension(const editor *e)
{
    const char *home = sys_env("USERPROFILE");
    if (!home) home = sys_env("HOME");
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
        } else {
            fprintf(stderr, "tide: couldn't add Tide to %s (%s --install-extension failed)\n", e->name, command);
            failed++;
        }
        free(command);
    }
    if (found == 0 && !only_updates) {
        printf("tide found no editor to add Tide to (VS Code, Cursor, VSCodium or Windsurf).\n"
               "For JetBrains IDEs and others, see https://github.com/BlenMiner/tide-engine#editors\n");
    }
    free(vsix);
    return failed > 0 ? 1 : 0;
}
