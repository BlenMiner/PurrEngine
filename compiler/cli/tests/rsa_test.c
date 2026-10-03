// RSA (rsa.c): a key OpenSSL made signs as OpenSSL does (PKCS #1 v1.5 is
// deterministic, so the signature is the same bytes), primes and composites
// that fool weaker tests are told apart, and keys made here go together.

#include <string.h>

#include "rsa.h"
#include "rsa_vectors.h"
#include "rtc.h"
#include "tide_test.h"

static void from_hex(const char *hex, rsa_number *out)
{
    out->size = strlen(hex) / 2;
    for (size_t i = 0; i < out->size; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            const char c = hex[2 * i + (size_t)k];
            v = v << 4 | (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out->bytes[i] = (uint8_t)v;
    }
}

static void from_bytes(const uint8_t *bytes, const size_t size, rsa_number *out)
{
    out->size = size;
    memcpy(out->bytes, bytes, size);
}

TIDE_TEST(rsa_signs_as_openssl_does)
{
    static rsa_key key;
    from_hex(N, &key.n);
    from_hex(E, &key.e);
    from_hex(D, &key.d);
    from_hex(P, &key.p);
    from_hex(Q, &key.q);
    from_hex(DP, &key.dp);
    from_hex(DQ, &key.dq);
    from_hex(QINV, &key.qinv);
    TIDE_REQUIRE(rsa_check(&key));
    TIDE_CHECK(rsa_size(&key.n) == 256);

    uint8_t hash[32], sig[256];
    rtc_sha256_of("abc", 3, hash);
    TIDE_REQUIRE(rsa_sign(&key, hash, sig));
    rsa_number want;
    from_hex(SIG, &want);
    TIDE_CHECK(memcmp(sig, want.bytes, sizeof sig) == 0);
    TIDE_CHECK(rsa_verify(&key.n, &key.e, sig, sizeof sig, hash));

    // A bit off in the signature, or in what was signed, and it isn't
    sig[100] ^= 0x10;
    TIDE_CHECK(!rsa_verify(&key.n, &key.e, sig, sizeof sig, hash));
    sig[100] ^= 0x10;
    hash[0] ^= 1;
    TIDE_CHECK(!rsa_verify(&key.n, &key.e, sig, sizeof sig, hash));

    // A key whose numbers don't go together
    key.dp.bytes[key.dp.size - 1] ^= 2;
    TIDE_CHECK(!rsa_check(&key));
}

TIDE_TEST(rsa_tells_primes)
{
    static const uint8_t two[] = {2}, small[] = {0x1e, 0xef}; // 7919
    static const uint8_t mersenne61[] = {0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t mersenne127[] = {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const struct {
        const uint8_t *bytes;
        size_t size;
    } primes[] = {{two, sizeof two}, {small, sizeof small}, {mersenne61, sizeof mersenne61},
                  {mersenne127, sizeof mersenne127}};
    for (size_t i = 0; i < sizeof primes / sizeof primes[0]; i++) {
        rsa_number n;
        from_bytes(primes[i].bytes, primes[i].size, &n);
        TIDE_CHECK(rsa_probably_prime(&n, 20));
    }
    rsa_number p, q;
    from_hex(P, &p);
    from_hex(Q, &q);
    TIDE_CHECK(rsa_probably_prime(&p, 20) && rsa_probably_prime(&q, 20));

    // 1, 561 (a Carmichael number), 2047 (a strong pseudoprime to base 2),
    // 3215031751 (one to bases 2, 3, 5 and 7), 2^128 + 1, and a key's modulus
    static const uint8_t one[] = {1}, carmichael[] = {0x02, 0x31}, base2[] = {0x07, 0xff};
    static const uint8_t bases[] = {0xbf, 0xa1, 0x7d, 0xc7};
    static const uint8_t fermat7[] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const struct {
        const uint8_t *bytes;
        size_t size;
    } composites[] = {{one, sizeof one},       {carmichael, sizeof carmichael}, {base2, sizeof base2},
                      {bases, sizeof bases},   {fermat7, sizeof fermat7}};
    for (size_t i = 0; i < sizeof composites / sizeof composites[0]; i++) {
        rsa_number n;
        from_bytes(composites[i].bytes, composites[i].size, &n);
        TIDE_CHECK(!rsa_probably_prime(&n, 20));
    }
    rsa_number n;
    from_hex(N, &n);
    TIDE_CHECK(!rsa_probably_prime(&n, 20));
}

TIDE_TEST(rsa_makes_keys)
{
    static rsa_key key;
    TIDE_REQUIRE(rsa_generate(&key, 1024));
    TIDE_CHECK(rsa_size(&key.n) == 128 && key.n.bytes[0] & 0x80);
    TIDE_CHECK(key.e.size == 3 && key.e.bytes[0] == 1 && key.e.bytes[1] == 0 && key.e.bytes[2] == 1);
    TIDE_CHECK(rsa_check(&key));
    TIDE_CHECK(rsa_probably_prime(&key.p, 20) && rsa_probably_prime(&key.q, 20));
    uint8_t hash[32], sig[128];
    rtc_sha256_of("tide", 4, hash);
    TIDE_REQUIRE(rsa_sign(&key, hash, sig));
    TIDE_CHECK(rsa_verify(&key.n, &key.e, sig, sizeof sig, hash));
    TIDE_CHECK(!rsa_generate(&key, 1000)); // Not a size it makes
}
