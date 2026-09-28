#pragma once

#include <stdbool.h>

// Adds PurrLang to the editors purr finds: the VS Code extension in the
// package (editors/purrlang.vsix) goes into VS Code, Cursor, VSCodium and
// Windsurf. With `only_updates`, only into those that already have it, as
// `purr upgrade` does. Returns the exit code.
int purr_editors(const char *root, bool only_updates);
