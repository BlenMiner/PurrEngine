#pragma once

#include <stdbool.h>

// tide's version, which `#if TIDE_0_3_OR_NEWER` and a package's `tide 0.3`
// line compare with. Only its numbers count: a pre-release, like
// 0.3.0-nightly.2, has what 0.3.0 will, so far.

// The version as written, like "0.3.0-nightly.2".
const char *tide_version_text(void);

// Whether this tide is major.minor.patch or newer.
bool tide_version_at_least(int major, int minor, int patch);

// Reads "0.3" or "0.3.1" (with nothing after it) into `out`, a missing patch
// as 0. False if `text` isn't one.
bool tide_version_parse(const char *text, int len, int out[3]);
