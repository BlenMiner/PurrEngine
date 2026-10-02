#pragma once

// Files on disk, kept apart so the server doesn't see windows.h.

typedef void (*folder_file_fn)(void *user, const char *path);

// Calls `found` with every file in `folder` (a path ending in '/') and its
// subfolders whose name ends with `extension`, such as ".tide". Paths are the
// folder followed by forward slashes. Links to other folders aren't followed.
void folder_find(const char *folder, const char *extension, folder_file_fn found, void *user);

// Like folder_find, but leaves out subfolders that hold a tide.packages file:
// each is a game or a package of its own (see packages.h).
void folder_find_own(const char *folder, const char *extension, folder_file_fn found, void *user);

// The folder this program is in, with forward slashes and ending in '/'.
// malloc'd; NULL if the OS won't say.
char *folder_of_program(void);
