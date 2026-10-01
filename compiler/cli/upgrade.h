#pragma once

#include <stdbool.h>

// Updating tide from GitHub Releases. Stable versions come from the release
// branch, nightly ones (pre-releases) from dev; both are published by
// .github/workflows/build.yml.

// The channel tide follows: "stable" or "nightly".
const char *tide_channel(const char *root);

// Installs the newest version of `channel` (NULL: the current one), or exactly
// `version` when it isn't NULL, over the installation in `root`. Returns the
// exit code.
int tide_upgrade(const char *root, const char *channel, const char *version);

// At most once a day, prints a line when a newer version is out. Quiet when
// offline, and off with TIDE_NO_UPDATE_CHECK.
void tide_check_for_update(const char *root);

// Deletes what an earlier upgrade left behind, such as a replaced tide.exe
// that was still running.
void tide_cleanup(const char *root);
