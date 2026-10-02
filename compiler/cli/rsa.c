// RSA (see rsa.h), written from RFC 8017, with numbers of 32-bit limbs and
// Montgomery's multiplication. Keys are made as FIPS 186-5 asks: two random
// primes of half the bits each, their top two bits set (so their product has
// all its bits), far enough apart, each prime to the exponent less one, and
// found with Miller-Rabin after dividing by the small primes. The random
// bytes are the system's (rtc_random).
//
// Nothing here runs anywhere an attacker could time it (tide signs on the
// machine it builds on), so it isn't constant-time.

#include "rsa.h"

#include <string.h>

#include "rtc.h"

// ---------------------------------------------------------------------------
// Numbers: little-endian limbs, `n` of them in use, the top one not zero

#define LIMBS (RSA_MAX_BYTES / 2 + 2) // The product of two of a key's numbers, and room to carry

typedef struct bn {
    int n;
    uint32_t w[LIMBS];
} bn;

static void trim(bn *a)
{
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

static void set_small(bn *a, const uint32_t v)
{
    a->w[0] = v;
    a->n = v != 0;
}

static void from_number(bn *a, const rsa_number *x)
{
    memset(a, 0, sizeof *a);
    for (size_t i = 0; i < x->size; i++) {
        const size_t at = x->size - 1 - i; // Bytes from the lowest
        a->w[i / 4] |= (uint32_t)x->bytes[at] << (8 * (i % 4));
    }
    a->n = (int)((x->size + 3) / 4);
    trim(a);
}

static int bits(const bn *a)
{
    if (a->n == 0) return 0;
    int b = 32 * (a->n - 1);
    for (uint32_t top = a->w[a->n - 1]; top; top >>= 1) b++;
    return b;
}

static bool bit(const bn *a, const int i)
{
    return i / 32 < a->n && ((a->w[i / 32] >> (i % 32)) & 1);
}

// The number in `size` bytes, big-endian, zeros in front
static void to_bytes(const bn *a, uint8_t *out, const size_t size)
{
    for (size_t i = 0; i < size; i++) {
        const size_t limb = i / 4;
        out[size - 1 - i] = limb < (size_t)a->n ? (uint8_t)(a->w[limb] >> (8 * (i % 4))) : 0;
    }
}

static void to_number(const bn *a, rsa_number *x)
{
    x->size = a->n ? (size_t)(bits(a) + 7) / 8 : 1;
    to_bytes(a, x->bytes, x->size);
}

static int compare(const bn *a, const bn *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    }
    return 0;
}

static void add(bn *r, const bn *a, const bn *b)
{
    const int n = a->n > b->n ? a->n : b->n;
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        carry += (uint64_t)(i < a->n ? a->w[i] : 0) + (i < b->n ? b->w[i] : 0);
        r->w[i] = (uint32_t)carry;
        carry >>= 32;
    }
    r->w[n] = (uint32_t)carry;
    r->n = n + 1;
    trim(r);
}

// r = a - b, for a >= b
static void sub(bn *r, const bn *a, const bn *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        borrow += (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0);
        r->w[i] = (uint32_t)borrow;
        borrow = borrow < 0 ? -1 : 0;
    }
    r->n = a->n;
    trim(r);
}

static void sub_small(bn *a, const uint32_t v)
{
    bn s;
    set_small(&s, v);
    sub(a, a, &s);
}

// r = a * b; r is neither
static void mul(bn *r, const bn *a, const bn *b)
{
    memset(r->w, 0, sizeof(uint32_t) * (size_t)(a->n + b->n + 1));
    for (int i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->n; j++) {
            carry += r->w[i + j] + (uint64_t)a->w[i] * b->w[j];
            r->w[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        r->w[i + b->n] = (uint32_t)carry;
    }
    r->n = a->n + b->n;
    trim(r);
}

// a = a * m + plus
static void mul_add_small(bn *a, const uint32_t m, const uint32_t plus)
{
    uint64_t carry = plus;
    for (int i = 0; i < a->n; i++) {
        carry += (uint64_t)a->w[i] * m;
        a->w[i] = (uint32_t)carry;
        carry >>= 32;
    }
    a->w[a->n++] = (uint32_t)carry;
    trim(a);
}

// a = a / d; returns the remainder
static uint32_t div_small(bn *a, const uint32_t d)
{
    uint64_t rest = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        rest = (rest << 32) | a->w[i];
        a->w[i] = (uint32_t)(rest / d);
        rest %= d;
    }
    trim(a);
    return (uint32_t)rest;
}

static uint32_t mod_small(const bn *a, const uint32_t d)
{
    uint64_t rest = 0;
    for (int i = a->n - 1; i >= 0; i--) rest = ((rest << 32) | a->w[i]) % d;
    return (uint32_t)rest;
}

// r = a mod m, a bit at a time: only for the few reductions outside
// Montgomery's (its constants, and a message into each prime's range).
static void mod(bn *r, const bn *a, const bn *m)
{
    bn rest = {0};
    for (int i = bits(a) - 1; i >= 0; i--) {
        // rest = 2 rest + the bit, less m when it's m or more
        uint32_t carry = bit(a, i);
        for (int k = 0; k < rest.n; k++) {
            const uint32_t top = rest.w[k] >> 31;
            rest.w[k] = (rest.w[k] << 1) | carry;
            carry = top;
        }
        if (carry) rest.w[rest.n++] = carry;
        if (compare(&rest, m) >= 0) sub(&rest, &rest, m);
    }
    *r = rest;
}

// ---------------------------------------------------------------------------
// Montgomery's multiplication, for an odd modulus m: numbers times R = 2^(32n)

typedef struct mont {
    bn m;
    int n;
    uint32_t inv; // -1/m mod 2^32
    bn rr;        // R^2 mod m: into Montgomery's form
} mont;

static void mont_init(mont *c, const bn *m)
{
    c->m = *m;
    c->n = m->n;
    // 1/m0 mod 2^32 by Newton's method: right to 3 bits from m0 itself
    // (odd squares are 1 mod 8), and each step doubles them.
    uint32_t x = m->w[0];
    for (int i = 0; i < 4; i++) x *= 2 - m->w[0] * x;
    c->inv = (uint32_t)0 - x;
    bn r2 = {0};
    r2.n = 2 * c->n + 1;
    r2.w[2 * c->n] = 1;
    mod(&c->rr, &r2, m);
}

// r = a b / R mod m, for a and b below m
static void mont_mul(const mont *c, bn *r, const bn *a, const bn *b)
{
    const int n = c->n;
    uint32_t t[LIMBS + 2] = {0};
    for (int i = 0; i < n; i++) {
        const uint32_t ai = i < a->n ? a->w[i] : 0;
        uint64_t carry = 0;
        for (int j = 0; j < n; j++) {
            carry += t[j] + (uint64_t)ai * (j < b->n ? b->w[j] : 0);
            t[j] = (uint32_t)carry;
            carry >>= 32;
        }
        carry += t[n];
        t[n] = (uint32_t)carry;
        t[n + 1] = (uint32_t)(carry >> 32);
        // Add a multiple of m that clears the lowest limb, and drop it
        const uint32_t u = t[0] * c->inv;
        carry = (t[0] + (uint64_t)u * c->m.w[0]) >> 32;
        for (int j = 1; j < n; j++) {
            carry += t[j] + (uint64_t)u * c->m.w[j];
            t[j - 1] = (uint32_t)carry;
            carry >>= 32;
        }
        carry += t[n];
        t[n - 1] = (uint32_t)carry;
        t[n] = t[n + 1] + (uint32_t)(carry >> 32);
    }
    memcpy(r->w, t, sizeof(uint32_t) * (size_t)(n + 1));
    r->n = n + 1;
    trim(r);
    if (compare(r, &c->m) >= 0) sub(r, r, &c->m);
}

static void mont_in(const mont *c, bn *r, const bn *a)
{
    mont_mul(c, r, a, &c->rr);
}

static void mont_out(const mont *c, bn *r, const bn *a)
{
    bn one;
    set_small(&one, 1);
    mont_mul(c, r, a, &one);
}

// r = base^exp mod m, for base below m
static void power(const mont *c, bn *r, const bn *base, const bn *exp)
{
    bn x, acc, one;
    mont_in(c, &x, base);
    set_small(&one, 1);
    mont_in(c, &acc, &one);
    for (int i = bits(exp) - 1; i >= 0; i--) {
        mont_mul(c, &acc, &acc, &acc);
        if (bit(exp, i)) mont_mul(c, &acc, &acc, &x);
    }
    mont_out(c, r, &acc);
}

// ---------------------------------------------------------------------------
// Primes

static bool random_bits(bn *a, const int count)
{
    memset(a, 0, sizeof *a);
    a->n = (count + 31) / 32;
    if (!rtc_random(a->w, sizeof(uint32_t) * (size_t)a->n)) return false;
    if (count % 32) a->w[a->n - 1] &= (1u << (count % 32)) - 1;
    trim(a);
    return true;
}

static bool miller_rabin(const bn *n, const int rounds)
{
    if (n->n == 0 || (n->n == 1 && n->w[0] < 4)) return n->n == 1 && n->w[0] >= 2;
    if (!(n->w[0] & 1)) return false;
    // n - 1 = d 2^s, d odd
    bn n1 = *n;
    sub_small(&n1, 1);
    int s = 0;
    while (!bit(&n1, s)) s++;
    bn d = {0};
    for (int i = s; i < bits(&n1); i++) {
        if (bit(&n1, i)) d.w[(i - s) / 32] |= 1u << ((i - s) % 32);
    }
    d.n = n1.n;
    trim(&d);
    mont c;
    mont_init(&c, n);
    bn one, two;
    set_small(&one, 1);
    set_small(&two, 2);
    for (int round = 0; round < rounds; round++) {
        bn a; // A random witness, from 2 to n - 2
        do {
            if (!random_bits(&a, bits(n) - 1)) return false;
        } while (compare(&a, &two) < 0);
        bn x;
        power(&c, &x, &a, &d);
        if (compare(&x, &one) == 0 || compare(&x, &n1) == 0) continue;
        bool passed = false;
        for (int i = 1; i < s && !passed; i++) {
            bn y;
            mont_in(&c, &y, &x);
            mont_mul(&c, &y, &y, &y);
            mont_out(&c, &x, &y);
            if (compare(&x, &one) == 0) return false;
            passed = compare(&x, &n1) == 0;
        }
        if (!passed) return false;
    }
    return true;
}

bool rsa_probably_prime(const rsa_number *n, const int rounds)
{
    bn a;
    from_number(&a, n);
    return miller_rabin(&a, rounds);
}

#define E 65537u
#define SIEVE 65536

static uint16_t small_primes[SIEVE / 8]; // The odd primes below SIEVE
static int small_count;

static void find_small_primes(void)
{
    if (small_count) return;
    static uint8_t composite[SIEVE];
    for (uint32_t i = 3; i < SIEVE; i += 2) {
        if (composite[i]) continue;
        small_primes[small_count++] = (uint16_t)i;
        for (uint32_t k = i * i; k < SIEVE; k += 2 * i) composite[k] = 1;
    }
}

// A random prime of exactly `count` bits, its top two set, and not 1 mod E
// (so E has an inverse mod p - 1): from a random odd start, the first after
// it that no small prime divides and that Miller-Rabin takes.
static bool random_prime(bn *p, const int count)
{
    find_small_primes();
    static uint32_t rest[SIEVE / 8];
    for (;;) {
        bn start;
        if (!random_bits(&start, count)) return false;
        start.w[(count - 1) / 32] |= 1u << ((count - 1) % 32);
        start.w[(count - 2) / 32] |= 1u << ((count - 2) % 32);
        start.w[0] |= 1;
        start.n = (count + 31) / 32;
        for (int i = 0; i < small_count; i++) rest[i] = mod_small(&start, small_primes[i]);
        const uint32_t rest_e = mod_small(&start, E);
        for (uint32_t delta = 0; delta < (1u << 20); delta += 2) {
            bool divides = false;
            for (int i = 0; i < small_count && !divides; i++) divides = (rest[i] + delta) % small_primes[i] == 0;
            if (divides || (rest_e + delta) % E == 1) continue;
            bn candidate = start, step;
            set_small(&step, delta);
            add(&candidate, &start, &step);
            if (bits(&candidate) != count) break;
            if (miller_rabin(&candidate, 32)) {
                *p = candidate;
                return true;
            }
        }
    }
}

// 1/a mod m, for a prime to m, by Euclid's algorithm
static uint32_t inverse_small(const uint32_t a, const uint32_t m)
{
    int64_t r0 = m, r1 = a, t0 = 0, t1 = 1;
    while (r1) {
        const int64_t q = r0 / r1, r2 = r0 - q * r1, t2 = t0 - q * t1;
        r0 = r1;
        r1 = r2;
        t0 = t1;
        t1 = t2;
    }
    return (uint32_t)(t0 < 0 ? t0 + m : t0);
}

// d = 1/E mod m: with k such that k m = -1 mod E, d = (k m + 1) / E, which
// divides exactly.
static bool inverse_e(bn *d, const bn *m)
{
    const uint32_t r = mod_small(m, E);
    if (r == 0) return false;
    const uint32_t k = E - inverse_small(r, E);
    *d = *m;
    mul_add_small(d, k, 1);
    return div_small(d, E) == 0;
}

bool rsa_generate(rsa_key *key, const int count)
{
    if (count < 1024 || count > 8 * RSA_MAX_BYTES || count % 64) return false;
    bn p, q, n;
    for (;;) {
        if (!random_prime(&p, count / 2) || !random_prime(&q, count / 2)) return false;
        if (compare(&p, &q) < 0) {
            const bn t = p;
            p = q;
            q = t;
        }
        bn gap;
        sub(&gap, &p, &q);
        mul(&n, &p, &q);
        if (bits(&gap) > count / 2 - 100 && bits(&n) == count) break;
    }
    bn e, p1 = p, q1 = q, phi, d, dp, dq, qinv, p2 = p;
    set_small(&e, E);
    sub_small(&p1, 1);
    sub_small(&q1, 1);
    mul(&phi, &p1, &q1);
    if (!inverse_e(&d, &phi) || !inverse_e(&dp, &p1) || !inverse_e(&dq, &q1)) return false;
    // 1/q mod p is q^(p-2), as p is prime
    sub_small(&p2, 2);
    mont cp;
    mont_init(&cp, &p);
    power(&cp, &qinv, &q, &p2);
    to_number(&n, &key->n);
    to_number(&e, &key->e);
    to_number(&d, &key->d);
    to_number(&p, &key->p);
    to_number(&q, &key->q);
    to_number(&dp, &key->dp);
    to_number(&dq, &key->dq);
    to_number(&qinv, &key->qinv);
    return rsa_check(key);
}

// ---------------------------------------------------------------------------
// Signatures

size_t rsa_size(const rsa_number *n)
{
    bn a;
    from_number(&a, n);
    return (size_t)(bits(&a) + 7) / 8;
}

// EMSA-PKCS1-v1_5 (RFC 8017, 9.2): 00 01 ff... 00, then SHA-256's DigestInfo
static bool encode(const uint8_t hash[32], const size_t size, bn *out)
{
    static const uint8_t info[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                   0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
    if (size < sizeof info + 32 + 11 || size > RSA_MAX_BYTES) return false;
    rsa_number em = {.size = size};
    em.bytes[1] = 0x01;
    memset(em.bytes + 2, 0xff, size - 3 - sizeof info - 32);
    memcpy(em.bytes + size - 32 - sizeof info, info, sizeof info);
    memcpy(em.bytes + size - 32, hash, 32);
    from_number(out, &em);
    return true;
}

bool rsa_verify(const rsa_number *n, const rsa_number *e, const uint8_t *sig, const size_t sig_size,
                const uint8_t hash[32])
{
    bn m, s, x, en, ee;
    from_number(&en, n);
    from_number(&ee, e);
    const size_t size = (size_t)(bits(&en) + 7) / 8;
    if (sig_size != size || !(en.w[0] & 1) || !encode(hash, size, &m)) return false;
    rsa_number given = {.size = sig_size};
    memcpy(given.bytes, sig, sig_size);
    from_number(&s, &given);
    if (compare(&s, &en) >= 0) return false;
    mont c;
    mont_init(&c, &en);
    power(&c, &x, &s, &ee);
    return compare(&x, &m) == 0;
}

bool rsa_sign(const rsa_key *key, const uint8_t hash[32], uint8_t *sig)
{
    bn n, p, q, dp, dq, qinv, m;
    from_number(&n, &key->n);
    from_number(&p, &key->p);
    from_number(&q, &key->q);
    from_number(&dp, &key->dp);
    from_number(&dq, &key->dq);
    from_number(&qinv, &key->qinv);
    const size_t size = (size_t)(bits(&n) + 7) / 8;
    if (!encode(hash, size, &m) || !(p.w[0] & 1) || !(q.w[0] & 1)) return false;
    // s = m^d mod n through its remainders: m1 mod p, m2 mod q, then
    // s = m2 + q (qinv (m1 - m2) mod p)
    mont cp, cq;
    mont_init(&cp, &p);
    mont_init(&cq, &q);
    bn mp, mq, m1, m2, m2p, h, hq, product, s;
    mod(&mp, &m, &p);
    mod(&mq, &m, &q);
    power(&cp, &m1, &mp, &dp);
    power(&cq, &m2, &mq, &dq);
    mod(&m2p, &m2, &p);
    if (compare(&m1, &m2p) < 0) add(&m1, &m1, &p);
    sub(&h, &m1, &m2p);
    mul(&product, &h, &qinv);
    mod(&h, &product, &p);
    mul(&hq, &h, &q);
    add(&s, &m2, &hq);
    if (compare(&s, &n) >= 0) return false;
    to_bytes(&s, sig, size);
    return rsa_verify(&key->n, &key->e, sig, size, hash);
}

bool rsa_check(const rsa_key *key)
{
    bn n, e, d, p, q, dp, dq, qinv, product, rest, p1, q1;
    from_number(&n, &key->n);
    from_number(&e, &key->e);
    from_number(&d, &key->d);
    from_number(&p, &key->p);
    from_number(&q, &key->q);
    from_number(&dp, &key->dp);
    from_number(&dq, &key->dq);
    from_number(&qinv, &key->qinv);
    if (n.n == 0 || p.n == 0 || q.n == 0 || e.n == 0 || bits(&n) > 8 * RSA_MAX_BYTES) return false;
    mul(&product, &p, &q);
    if (compare(&product, &n) != 0) return false;
    p1 = p;
    q1 = q;
    sub_small(&p1, 1);
    sub_small(&q1, 1);
    // e d = 1 mod p - 1 and q - 1, and dp, dq are d's remainders
    mul(&product, &e, &d);
    mod(&rest, &product, &p1);
    if (rest.n != 1 || rest.w[0] != 1) return false;
    mod(&rest, &product, &q1);
    if (rest.n != 1 || rest.w[0] != 1) return false;
    mod(&rest, &d, &p1);
    if (compare(&rest, &dp) != 0) return false;
    mod(&rest, &d, &q1);
    if (compare(&rest, &dq) != 0) return false;
    mul(&product, &qinv, &q);
    mod(&rest, &product, &p);
    return rest.n == 1 && rest.w[0] == 1;
}
