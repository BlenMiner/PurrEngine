#pragma once

#include <stddef.h>
#include <stdint.h>

// SHA-256 (FIPS 180-4), for what needs a hash nobody can work back from:
// the digests of players' cookies that sessions share for host migration
// (tide/session.h), and WebRTC's crypto (platform/src/rtc).
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

typedef struct tide_sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
} tide_sha256;

void tide_sha256_init(tide_sha256 *s);
void tide_sha256_add(tide_sha256 *s, const void *data, size_t size);
void tide_sha256_end(tide_sha256 *s, uint8_t out[32]);
void tide_sha256_of(const void *data, size_t size, uint8_t out[32]);
