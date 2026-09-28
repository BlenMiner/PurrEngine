#pragma once

#include <stddef.h>

// The SHA-256 of `data` as 64 lowercase hex digits and a NUL, for checking
// downloads against a release's SHA256SUMS.
void sha256_hex(const void *data, size_t len, char out[65]);
