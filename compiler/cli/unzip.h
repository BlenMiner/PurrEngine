#pragma once

// Zip archives, read with no tool of the system's: Google ships Android's NDK
// and platform tools as zips, which Linux's tar can't open. Entries stored or
// deflated, ZIP64's large archives too, written from PKWARE's APPNOTE and
// RFC 1951.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Whether to extract the entry called `name` (its path in the archive, with
// forward slashes), and where to: its path under the folder unzip extracts to,
// written to `to` (`size` bytes). False skips it.
typedef bool (*unzip_filter)(void *user, const char *name, char *to, size_t size);

// Extracts the entries `wanted` takes from `archive` into `dir`. Files keep
// whether they can run, on systems that say so. False after saying what's
// wrong.
bool unzip(const char *archive, const char *dir, unzip_filter wanted, void *user);

// RFC 1951's DEFLATE, decompressed: `in` into `out`, which holds exactly
// `out_size` bytes. False if it's not that much, or not DEFLATE.
bool unzip_inflate(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size);
