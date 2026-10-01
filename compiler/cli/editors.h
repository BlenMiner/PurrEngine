#pragma once

#include <stdbool.h>

// Adds Tide to the editors tide finds: the VS Code extension in the
// package (editors/tide.vsix) goes into VS Code, Cursor, VSCodium and
// Windsurf. With `only_updates`, only into those that already have it, as
// `tide upgrade` does. Returns the exit code.
int tide_editors(const char *root, bool only_updates);
