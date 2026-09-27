#pragma once

// Lists files on disk, kept apart so the server doesn't see windows.h.

typedef void (*folder_file_fn)(void *user, const char *path);

// Calls `found` with every file in `folder` (a path ending in '/') and its
// subfolders whose name ends with `extension`, such as ".purr". Paths are the
// folder followed by forward slashes. Links to other folders aren't followed.
void folder_find(const char *folder, const char *extension, folder_file_fn found, void *user);
