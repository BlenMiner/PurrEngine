#pragma once

#include <stdbool.h>

#include "json.h"

// Versions and GitHub releases, for tide upgrade and the installers' logic.

// Versions as semantic-release makes them (.releaserc.json): 1.2.3, and
// pre-releases like 1.2.3-nightly.4. Returns less than, equal to or more than
// 0 as `a` is older than, the same as or newer than `b`, by semantic
// versioning's rules: a pre-release comes before its release, and nightly.10
// after nightly.9. Anything else is older than every version.
int tide_version_compare(const char *a, const char *b);
// Whether `text` is a version as above.
bool tide_version_valid(const char *text);

// A release's version: its tag, without the leading 'v'.
const char *tide_release_version(const json *release);

// What `channel` ("stable" or "nightly") installs: of `releases` (GitHub's
// list) and `latest` (GitHub's latest release, or NULL), the highest version
// that isn't a draft and, for stable, isn't a pre-release. NULL if none is.
const json *tide_release_pick(const json *releases, const json *latest, const char *channel);

// Whether `release` is published (not a draft) and of `version`, written with
// or without its 'v'.
bool tide_release_is(const json *release, const char *version);
// The release of `version` in `releases`, or NULL.
const json *tide_release_find(const json *releases, const char *version);
