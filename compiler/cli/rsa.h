#pragma once

// RSA, as Android apps and Google Play's upload keys need it (RFC 8017):
// making keys, and signatures with PKCS #1 v1.5 over SHA-256. Play takes
// upload keys of RSA only, so tide signs every app with one (see sign.h).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSA_MAX_BYTES 512 // Keys of up to 4096 bits

// An unsigned number, big-endian, as DER's INTEGERs hold it
typedef struct rsa_number {
    size_t size;
    uint8_t bytes[RSA_MAX_BYTES];
} rsa_number;

// A private key, with the numbers that make signing quick (the Chinese
// remainder theorem's), as PKCS #1's RSAPrivateKey has them.
typedef struct rsa_key {
    rsa_number n, e, d, p, q, dp, dq, qinv;
} rsa_key;

// The modulus's size in bytes: a signature's.
size_t rsa_size(const rsa_number *n);

// Makes a key of `bits` bits (2048 for Play), its exponent 65537.
bool rsa_generate(rsa_key *key, int bits);

// Signs the SHA-256 `hash` (PKCS #1 v1.5) into `sig`, rsa_size(&key->n)
// bytes. Each signature is checked before it's given.
bool rsa_sign(const rsa_key *key, const uint8_t hash[32], uint8_t *sig);

// Whether `sig` is the key's signature of `hash`.
bool rsa_verify(const rsa_number *n, const rsa_number *e, const uint8_t *sig, size_t sig_size, const uint8_t hash[32]);

// Whether the key's numbers go together: n = pq, and d, dp, dq and qinv are
// what they should be for them.
bool rsa_check(const rsa_key *key);

// Whether `n` is prime, as far as `rounds` rounds of Miller-Rabin can tell.
bool rsa_probably_prime(const rsa_number *n, int rounds);
