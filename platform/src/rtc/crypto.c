// Hashes, HMAC, TLS 1.2's PRF, ChaCha20-Poly1305, CRCs and random bytes for
// WebRTC (see rtc.h), each written from its specification.

#if defined(__linux__)
#define _DEFAULT_SOURCE // getrandom
#endif

#include "rtc.h"

#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// RtlGenRandom, from advapi32, which games link already
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG size);
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <errno.h>
#include <sys/random.h>
#endif

static uint32_t rol(const uint32_t x, const int n)
{
    return x << n | x >> (32 - n);
}

static uint32_t ror(const uint32_t x, const int n)
{
    return x >> n | x << (32 - n);
}

static uint32_t get32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32le(uint8_t *p, const uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = rtc_get32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

void rtc_sha256_init(rtc_sha256 *s)
{
    static const uint32_t start[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, start, sizeof start);
    s->bytes = 0;
}

void rtc_sha256_add(rtc_sha256 *s, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size) {
        const size_t used = (size_t)(s->bytes % 64u);
        const size_t n = size < 64 - used ? size : 64 - used;
        memcpy(s->block + used, p, n);
        s->bytes += n;
        p += n;
        size -= n;
        if (used + n == 64) sha256_block(s->h, s->block);
    }
}

void rtc_sha256_end(rtc_sha256 *s, uint8_t out[32])
{
    const uint64_t bits = s->bytes * 8u;
    static const uint8_t pad[64] = {0x80};
    const size_t used = (size_t)(s->bytes % 64u);
    rtc_sha256_add(s, pad, used < 56 ? 56 - used : 120 - used);
    uint8_t length[8];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - 8 * i));
    rtc_sha256_add(s, length, 8);
    for (int i = 0; i < 8; i++) rtc_put32(out + 4 * i, s->h[i]);
}

void rtc_sha256_of(const void *data, const size_t size, uint8_t out[32])
{
    rtc_sha256 s;
    rtc_sha256_init(&s);
    rtc_sha256_add(&s, data, size);
    rtc_sha256_end(&s, out);
}

// ---------------------------------------------------------------------------
// SHA-1 (FIPS 180-4), for STUN's MESSAGE-INTEGRITY

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = rtc_get32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void rtc_sha1_init(rtc_sha1 *s)
{
    static const uint32_t start[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    memcpy(s->h, start, sizeof start);
    s->bytes = 0;
}

void rtc_sha1_add(rtc_sha1 *s, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size) {
        const size_t used = (size_t)(s->bytes % 64u);
        const size_t n = size < 64 - used ? size : 64 - used;
        memcpy(s->block + used, p, n);
        s->bytes += n;
        p += n;
        size -= n;
        if (used + n == 64) sha1_block(s->h, s->block);
    }
}

void rtc_sha1_end(rtc_sha1 *s, uint8_t out[20])
{
    const uint64_t bits = s->bytes * 8u;
    static const uint8_t pad[64] = {0x80};
    const size_t used = (size_t)(s->bytes % 64u);
    rtc_sha1_add(s, pad, used < 56 ? 56 - used : 120 - used);
    uint8_t length[8];
    for (int i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - 8 * i));
    rtc_sha1_add(s, length, 8);
    for (int i = 0; i < 5; i++) rtc_put32(out + 4 * i, s->h[i]);
}

// ---------------------------------------------------------------------------
// MD5 (RFC 1321)

static const uint32_t K_MD5[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t S_MD5[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t h[4], const uint8_t *p)
{
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = get32le(p + 4 * i);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f += a + K_MD5[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += rol(f, S_MD5[i]);
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

void rtc_md5(const void *data, const size_t size, uint8_t out[16])
{
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t *p = data;
    size_t left = size;
    for (; left >= 64; left -= 64, p += 64) md5_block(h, p);
    uint8_t tail[128] = {0};
    memcpy(tail, p, left);
    tail[left] = 0x80;
    const size_t end = left < 56 ? 64 : 128;
    const uint64_t bits = (uint64_t)size * 8u;
    for (int i = 0; i < 8; i++) tail[end - 8 + (size_t)i] = (uint8_t)(bits >> (8 * i));
    md5_block(h, tail);
    if (end == 128) md5_block(h, tail + 64);
    for (int i = 0; i < 4; i++) put32le(out + 4 * i, h[i]);
}

// ---------------------------------------------------------------------------
// HMAC (RFC 2104)

void rtc_hmac256_init(rtc_hmac256 *h, const void *key, const size_t key_size)
{
    uint8_t k[64] = {0};
    if (key_size > 64) rtc_sha256_of(key, key_size, k);
    else memcpy(k, key, key_size);
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    rtc_sha256_init(&h->inner);
    rtc_sha256_add(&h->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    rtc_sha256_init(&h->outer);
    rtc_sha256_add(&h->outer, pad, 64);
}

void rtc_hmac256_add(rtc_hmac256 *h, const void *data, const size_t size)
{
    rtc_sha256_add(&h->inner, data, size);
}

void rtc_hmac256_end(rtc_hmac256 *h, uint8_t out[32])
{
    uint8_t inner[32];
    rtc_sha256_end(&h->inner, inner);
    rtc_sha256_add(&h->outer, inner, 32);
    rtc_sha256_end(&h->outer, out);
}

void rtc_hmac1_init(rtc_hmac1 *h, const void *key, const size_t key_size)
{
    uint8_t k[64] = {0};
    if (key_size > 64) {
        rtc_sha1 s;
        rtc_sha1_init(&s);
        rtc_sha1_add(&s, key, key_size);
        rtc_sha1_end(&s, k);
    } else {
        memcpy(k, key, key_size);
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    rtc_sha1_init(&h->inner);
    rtc_sha1_add(&h->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    rtc_sha1_init(&h->outer);
    rtc_sha1_add(&h->outer, pad, 64);
}

void rtc_hmac1_add(rtc_hmac1 *h, const void *data, const size_t size)
{
    rtc_sha1_add(&h->inner, data, size);
}

void rtc_hmac1_end(rtc_hmac1 *h, uint8_t out[20])
{
    uint8_t inner[20];
    rtc_sha1_end(&h->inner, inner);
    rtc_sha1_add(&h->outer, inner, 20);
    rtc_sha1_end(&h->outer, out);
}

// P_SHA256(secret, label + seed): A(1) = HMAC(secret, label + seed), then
// blocks of HMAC(secret, A(i) + label + seed), with A(i+1) = HMAC(secret, A(i)).
void rtc_prf(const uint8_t *secret, const size_t secret_size, const char *label, const uint8_t *seed1,
             const size_t seed1_size, const uint8_t *seed2, const size_t seed2_size, uint8_t *out, size_t size)
{
    const size_t label_size = strlen(label);
    uint8_t a[32];
    rtc_hmac256 h;
    rtc_hmac256_init(&h, secret, secret_size);
    rtc_hmac256_add(&h, label, label_size);
    rtc_hmac256_add(&h, seed1, seed1_size);
    rtc_hmac256_add(&h, seed2, seed2_size);
    rtc_hmac256_end(&h, a);
    while (size) {
        uint8_t block[32];
        rtc_hmac256_init(&h, secret, secret_size);
        rtc_hmac256_add(&h, a, 32);
        rtc_hmac256_add(&h, label, label_size);
        rtc_hmac256_add(&h, seed1, seed1_size);
        rtc_hmac256_add(&h, seed2, seed2_size);
        rtc_hmac256_end(&h, block);
        const size_t n = size < 32 ? size : 32;
        memcpy(out, block, n);
        out += n;
        size -= n;
        rtc_hmac256_init(&h, secret, secret_size);
        rtc_hmac256_add(&h, a, 32);
        rtc_hmac256_end(&h, a);
    }
}

// ---------------------------------------------------------------------------
// ChaCha20 and Poly1305 (RFC 8439)

#define QUARTER(a, b, c, d)                                                                                            \
    a += b;                                                                                                            \
    d = rol(d ^ a, 16);                                                                                                \
    c += d;                                                                                                            \
    b = rol(b ^ c, 12);                                                                                                \
    a += b;                                                                                                            \
    d = rol(d ^ a, 8);                                                                                                 \
    c += d;                                                                                                            \
    b = rol(b ^ c, 7)

static void chacha20_block(const uint8_t key[32], const uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) s[4 + i] = get32le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = get32le(nonce + 4 * i);
    uint32_t x[16];
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QUARTER(x[0], x[4], x[8], x[12]);
        QUARTER(x[1], x[5], x[9], x[13]);
        QUARTER(x[2], x[6], x[10], x[14]);
        QUARTER(x[3], x[7], x[11], x[15]);
        QUARTER(x[0], x[5], x[10], x[15]);
        QUARTER(x[1], x[6], x[11], x[12]);
        QUARTER(x[2], x[7], x[8], x[13]);
        QUARTER(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put32le(out + 4 * i, x[i] + s[i]);
}

// XORs the key stream from block `counter` on into `size` bytes.
static void chacha20(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out,
                     size_t size)
{
    uint8_t stream[64];
    while (size) {
        chacha20_block(key, counter++, nonce, stream);
        const size_t n = size < 64 ? size : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ stream[i];
        in += n;
        out += n;
        size -= n;
    }
}

// Poly1305 in 26-bit limbs, so every product fits 64 bits.
typedef struct poly1305 {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
} poly1305;

static void poly1305_init(poly1305 *p, const uint8_t key[32])
{
    p->r[0] = get32le(key + 0) & 0x3ffffff;
    p->r[1] = (get32le(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (get32le(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (get32le(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (get32le(key + 12) >> 8) & 0x00fffff;
    memset(p->h, 0, sizeof p->h);
    for (int i = 0; i < 4; i++) p->pad[i] = get32le(key + 16 + 4 * i);
}

// One 16-byte block. The AEAD construction pads every part to whole blocks,
// so each has its 2^128 bit.
static void poly1305_block(poly1305 *p, const uint8_t m[16])
{
    const uint32_t hibit = 1u << 24;
    const uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0] + (get32le(m + 0) & 0x3ffffff);
    uint32_t h1 = p->h[1] + ((get32le(m + 3) >> 2) & 0x3ffffff);
    uint32_t h2 = p->h[2] + ((get32le(m + 6) >> 4) & 0x3ffffff);
    uint32_t h3 = p->h[3] + ((get32le(m + 9) >> 6) & 0x3ffffff);
    uint32_t h4 = p->h[4] + ((get32le(m + 12) >> 8) | hibit);
    const uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
    uint32_t c = (uint32_t)(d0 >> 26);
    h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c;
    c = (uint32_t)(d1 >> 26);
    h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c;
    c = (uint32_t)(d2 >> 26);
    h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c;
    c = (uint32_t)(d3 >> 26);
    h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c;
    c = (uint32_t)(d4 >> 26);
    h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;
    p->h[0] = h0;
    p->h[1] = h1;
    p->h[2] = h2;
    p->h[3] = h3;
    p->h[4] = h4;
}

// The message in 16-byte blocks, the last one padded with zeros, as the AEAD
// construction pads each part.
static void poly1305_padded(poly1305 *p, const uint8_t *m, size_t size)
{
    for (; size >= 16; size -= 16, m += 16) poly1305_block(p, m);
    if (size) {
        uint8_t block[16] = {0};
        memcpy(block, m, size);
        poly1305_block(p, block);
    }
}

static void poly1305_end(poly1305 *p, uint8_t tag[16])
{
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    uint32_t c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffff;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffff;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;

    // h - p, kept if it's not negative
    uint32_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffff;
    const uint32_t g4 = h4 + c - (1u << 26);
    const uint32_t keep_g = (g4 >> 31) - 1u; // All ones when g4 didn't go below zero
    h0 = (h0 & ~keep_g) | (g0 & keep_g);
    h1 = (h1 & ~keep_g) | (g1 & keep_g);
    h2 = (h2 & ~keep_g) | (g2 & keep_g);
    h3 = (h3 & ~keep_g) | (g3 & keep_g);
    h4 = (h4 & ~keep_g) | (g4 & keep_g);

    // 130 bits to 128, plus the pad
    const uint32_t w0 = h0 | h1 << 26;
    const uint32_t w1 = h1 >> 6 | h2 << 20;
    const uint32_t w2 = h2 >> 12 | h3 << 14;
    const uint32_t w3 = h3 >> 18 | h4 << 8;
    uint64_t f = (uint64_t)w0 + p->pad[0];
    put32le(tag + 0, (uint32_t)f);
    f = (uint64_t)w1 + p->pad[1] + (f >> 32);
    put32le(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + p->pad[2] + (f >> 32);
    put32le(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + p->pad[3] + (f >> 32);
    put32le(tag + 12, (uint32_t)f);
}

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, const size_t ad_size,
                     const uint8_t *sealed, const size_t size, uint8_t tag[16])
{
    uint8_t block[64];
    chacha20_block(key, 0, nonce, block);
    poly1305 p;
    poly1305_init(&p, block);
    poly1305_padded(&p, ad, ad_size);
    poly1305_padded(&p, sealed, size);
    uint8_t lengths[16];
    for (int i = 0; i < 8; i++) {
        lengths[i] = (uint8_t)((uint64_t)ad_size >> (8 * i));
        lengths[8 + i] = (uint8_t)((uint64_t)size >> (8 * i));
    }
    poly1305_block(&p, lengths);
    poly1305_end(&p, tag);
}

void rtc_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, const size_t ad_size,
              const uint8_t *text, const size_t size, uint8_t *out)
{
    chacha20(key, 1, nonce, text, out, size);
    aead_tag(key, nonce, ad, ad_size, out, size, out + size);
}

bool rtc_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, const size_t ad_size,
              const uint8_t *sealed, const size_t sealed_size, uint8_t *out)
{
    if (sealed_size < 16) return false;
    const size_t size = sealed_size - 16;
    uint8_t tag[16];
    aead_tag(key, nonce, ad, ad_size, sealed, size, tag);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(tag[i] ^ sealed[size + (size_t)i]);
    if (diff) return false;
    chacha20(key, 1, nonce, sealed, out, size);
    return true;
}

// ---------------------------------------------------------------------------
// CRCs, bit by bit: packets are small, and it keeps them free of tables.

static uint32_t crc(const void *data, const size_t size, const uint32_t poly)
{
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < size; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (poly & (0u - (c & 1u)));
    }
    return ~c;
}

uint32_t rtc_crc32(const void *data, const size_t size)
{
    return crc(data, size, 0xedb88320u);
}

uint32_t rtc_crc32c(const void *data, const size_t size)
{
    return crc(data, size, 0x82f63b78u);
}

// ---------------------------------------------------------------------------

bool rtc_random(void *out, size_t size)
{
#if defined(_WIN32)
    uint8_t *p = out;
    while (size) {
        const ULONG n = size > 0x10000000u ? 0x10000000u : (ULONG)size;
        if (!SystemFunction036(p, n)) return false;
        p += n;
        size -= n;
    }
    return true;
#elif defined(__APPLE__)
    arc4random_buf(out, size);
    return true;
#else
    uint8_t *p = out;
    while (size) {
        const ssize_t n = getrandom(p, size, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        size -= (size_t)n;
    }
    return true;
#endif
}
