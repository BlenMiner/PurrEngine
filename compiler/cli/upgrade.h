#pragma once

#include <stdbool.h>

// Updating purr from GitHub Releases. Stable versions come from the release
// branch, nightly ones (pre-releases) from dev; both are published by
// .github/workflows/build.yml.

// The channel purr follows: "stable" or "nightly".
const char *purr_channel(const char *root);

// Installs the newest version of `channel` (NULL: the current one), or exactly
// `version` when it isn't NULL, over the installation in `root`. Returns the
// exit code.
int purr_upgrade(const char *root, const char *channel, const char *version);

// At most once a day, prints a line when a newer version is out. Quiet when
// offline, and off with PURR_NO_UPDATE_CHECK.
void purr_check_for_update(const char *root);

// Deletes what an earlier upgrade left behind, such as a replaced purr.exe
// that was still running.
void purr_cleanup(const char *root);
