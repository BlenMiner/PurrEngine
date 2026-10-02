#pragma once

// `tide add` and `tide update`: changing the tide.packages of the game in
// `folder` (see packages.h). Both add the lines of what its packages need
// that it doesn't list yet. Return the exit code.

// Adds a package: from git (a URL or a source, with @ref to follow a branch
// or version, at that ref's newest commit), or a folder on this machine.
int tide_add(const char *folder, const char *what);

// Moves each named package (by its name, its repository's, or its source),
// or with none every package from git, to the newest commit of what it
// follows.
int tide_update(const char *folder, const char *const *names, int count);
