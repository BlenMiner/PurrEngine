// P-256 (secp256r1, FIPS 186-4 D.1.2.3) for WebRTC's DTLS: key pairs, ECDH
// and ECDSA with SHA-256 (see rtc.h).
//
// Chosen to be simple rather than fast, since a connection only needs a
// handful of these: numbers are 8 32-bit limbs, least significant first, in
// Montgomery form, and points are projective, added with the complete
// formulas of Renes, Costello and Batina (2016, algorithm 4), which also
// double. Anything touching a secret runs the same way whatever its value:
// selects are masks, and scalars are multiplied bit by bit, adding every time.

#include "rtc.h"

#include <string.h>

typedef uint32_t num[8];

typedef struct modulus {
    num m;
    num rr;       // R^2 mod m, for R = 2^256: into Montgomery form
    num one;      // R mod m: 1 in Montgomery form
    uint32_t inv; // -m^-1 mod 2^32
} modulus;

static modulus P; // The field's prime
static modulus N; // The group's order
static num B;     // The curve's b, in Montgomery form (mod P)
static num GX, GY; // The base point, in Montgomery form

// ---------------------------------------------------------------------------
// Numbers

static void from_bytes(num r, const uint8_t in[32])
{
    for (int i = 0; i < 8; i++) r[i] = rtc_get32(in + 28 - 4 * i);
}

static void to_bytes(uint8_t out[32], const num a)
{
    for (int i = 0; i < 8; i++) rtc_put32(out + 28 - 4 * i, a[i]);
}

// All ones if `bit`, else zero.
static uint32_t mask_of(const uint32_t bit)
{
    return 0u - bit;
}

static void select_num(num r, const uint32_t mask, const num a, const num b) // mask ? a : b
{
    for (int i = 0; i < 8; i++) r[i] = (a[i] & mask) | (b[i] & ~mask);
}

static uint32_t add_raw(num r, const num a, const num b) // Carry out
{
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

static uint32_t sub_raw(num r, const num a, const num b) // Borrow out
{
    int64_t c = 0;
    for (int i = 0; i < 8; i++) {
        c += (int64_t)a[i] - b[i];
        r[i] = (uint32_t)c;
        c >>= 32; // Arithmetic: 0 or -1
    }
    return (uint32_t)(-c);
}

static bool is_zero(const num a)
{
    uint32_t any = 0;
    for (int i = 0; i < 8; i++) any |= a[i];
    return any == 0;
}

static bool equal(const num a, const num b)
{
    uint32_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

// a < b
static bool less(const num a, const num b)
{
    num t;
    return sub_raw(t, a, b) != 0;
}

static void add_mod(num r, const num a, const num b, const modulus *m)
{
    num sum, less_m;
    const uint32_t carry = add_raw(sum, a, b);
    const uint32_t borrow = sub_raw(less_m, sum, m->m);
    // The sum went past 2^256, or it's at least m: take m off
    select_num(r, mask_of(carry | (borrow ^ 1u)), less_m, sum);
}

static void sub_mod(num r, const num a, const num b, const modulus *m)
{
    num diff, plus_m;
    const uint32_t borrow = sub_raw(diff, a, b);
    add_raw(plus_m, diff, m->m);
    select_num(r, mask_of(borrow), plus_m, diff);
}

// a * b / R mod m (CIOS)
static void mont_mul(num r, const num a, const num b, const modulus *m)
{
    uint32_t t[10] = {0};
    for (int i = 0; i < 8; i++) {
        uint64_t c = 0;
        for (int j = 0; j < 8; j++) {
            c += (uint64_t)t[j] + (uint64_t)a[j] * b[i];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[8];
        t[8] = (uint32_t)c;
        t[9] = (uint32_t)(c >> 32);
        const uint32_t u = t[0] * m->inv;
        c = ((uint64_t)t[0] + (uint64_t)u * m->m[0]) >> 32;
        for (int j = 1; j < 8; j++) {
            c += (uint64_t)t[j] + (uint64_t)u * m->m[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        c += t[8];
        t[7] = (uint32_t)c;
        t[8] = t[9] + (uint32_t)(c >> 32);
    }
    num less_m;
    const uint32_t borrow = sub_raw(less_m, t, m->m);
    select_num(r, mask_of((t[8] != 0) | (borrow ^ 1u)), less_m, t);
}

static void to_mont(num r, const num a, const modulus *m)
{
    mont_mul(r, a, m->rr, m);
}

static void from_mont(num r, const num a, const modulus *m)
{
    static const num one = {1};
    mont_mul(r, a, one, m);
}

// a^-1, both in Montgomery form: a^(m-2), whose bits aren't secret.
static void inverse(num r, const num a, const modulus *m)
{
    static const num two = {2};
    num e, x;
    sub_raw(e, m->m, two);
    memcpy(x, m->one, sizeof x);
    for (int i = 255; i >= 0; i--) {
        mont_mul(x, x, x, m);
        if (e[i / 32] >> (i % 32) & 1u) mont_mul(x, x, a, m);
    }
    memcpy(r, x, sizeof x);
}

// A value below 2^256 that may be up to twice m, reduced below it.
static void reduce_once(num r, const num a, const modulus *m)
{
    num less_m;
    const uint32_t borrow = sub_raw(less_m, a, m->m);
    select_num(r, mask_of(borrow ^ 1u), less_m, a);
}

static void init_modulus(modulus *m, const num value)
{
    memcpy(m->m, value, sizeof(num));
    uint32_t x = 1; // m[0]^-1 mod 2^32, by Newton's method
    for (int i = 0; i < 5; i++) x *= 2u - m->m[0] * x;
    m->inv = 0u - x;
    num r = {1};
    for (int i = 0; i < 256; i++) add_mod(r, r, r, m);
    memcpy(m->one, r, sizeof r);
    for (int i = 0; i < 256; i++) add_mod(r, r, r, m);
    memcpy(m->rr, r, sizeof r);
}

static void init(void)
{
    static bool done;
    if (done) return;
    static const num p = {0xffffffff, 0xffffffff, 0xffffffff, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0xffffffff};
    static const num n = {0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad, 0xffffffff, 0xffffffff, 0x00000000, 0xffffffff};
    static const num b = {0x27d2604b, 0x3bce3c3e, 0xcc53b0f6, 0x651d06b0, 0x769886bc, 0xb3ebbd55, 0xaa3a93e7, 0x5ac635d8};
    static const num gx = {0xd898c296, 0xf4a13945, 0x2deb33a0, 0x77037d81, 0x63a440f2, 0xf8bce6e5, 0xe12c4247, 0x6b17d1f2};
    static const num gy = {0x37bf51f5, 0xcbb64068, 0x6b315ece, 0x2bce3357, 0x7c0f9e16, 0x8ee7eb4a, 0xfe1a7f9b, 0x4fe342e2};
    init_modulus(&P, p);
    init_modulus(&N, n);
    to_mont(B, b, &P);
    to_mont(GX, gx, &P);
    to_mont(GY, gy, &P);
    done = true;
}

// ---------------------------------------------------------------------------
// Points: projective (X : Y : Z), Montgomery form; (0 : 1 : 0) is infinity.

typedef struct point {
    num x, y, z;
} point;

#define FMUL(r, a, b) mont_mul(r, a, b, &P)
#define FADD(r, a, b) add_mod(r, a, b, &P)
#define FSUB(r, a, b) sub_mod(r, a, b, &P)

// Complete addition for a = -3 (Renes, Costello, Batina: algorithm 4). It
// adds any two points, the same or infinity included. `r` may be `p` or `q`.
static void point_add(point *r, const point *p, const point *q)
{
    num t0, t1, t2, t3, t4, x3, y3, z3;
    FMUL(t0, p->x, q->x);
    FMUL(t1, p->y, q->y);
    FMUL(t2, p->z, q->z);
    FADD(t3, p->x, p->y);
    FADD(t4, q->x, q->y);
    FMUL(t3, t3, t4);
    FADD(t4, t0, t1);
    FSUB(t3, t3, t4);
    FADD(t4, p->y, p->z);
    FADD(x3, q->y, q->z);
    FMUL(t4, t4, x3);
    FADD(x3, t1, t2);
    FSUB(t4, t4, x3);
    FADD(x3, p->x, p->z);
    FADD(y3, q->x, q->z);
    FMUL(x3, x3, y3);
    FADD(y3, t0, t2);
    FSUB(y3, x3, y3);
    FMUL(z3, B, t2);
    FSUB(x3, y3, z3);
    FADD(z3, x3, x3);
    FADD(x3, x3, z3);
    FSUB(z3, t1, x3);
    FADD(x3, t1, x3);
    FMUL(y3, B, y3);
    FADD(t1, t2, t2);
    FADD(t2, t1, t2);
    FSUB(y3, y3, t2);
    FSUB(y3, y3, t0);
    FADD(t1, y3, y3);
    FADD(y3, t1, y3);
    FADD(t1, t0, t0);
    FADD(t0, t1, t0);
    FSUB(t0, t0, t2);
    FMUL(t1, t4, y3);
    FMUL(t2, t0, y3);
    FMUL(y3, x3, z3);
    FADD(y3, y3, t2);
    FMUL(x3, x3, t3);
    FSUB(x3, x3, t1);
    FMUL(z3, z3, t4);
    FMUL(t1, t3, t0);
    FADD(z3, z3, t1);
    memcpy(r->x, x3, sizeof x3);
    memcpy(r->y, y3, sizeof y3);
    memcpy(r->z, z3, sizeof z3);
}

// k times p, for a scalar k below the order.
static void point_mul(point *r, const num k, const point *p)
{
    point acc, sum;
    memset(acc.x, 0, sizeof acc.x);
    memcpy(acc.y, P.one, sizeof acc.y);
    memset(acc.z, 0, sizeof acc.z);
    for (int i = 255; i >= 0; i--) {
        point_add(&acc, &acc, &acc);
        point_add(&sum, &acc, p);
        const uint32_t mask = mask_of(k[i / 32] >> (i % 32) & 1u);
        select_num(acc.x, mask, sum.x, acc.x);
        select_num(acc.y, mask, sum.y, acc.y);
        select_num(acc.z, mask, sum.z, acc.z);
    }
    *r = acc;
}

// x and y, out of Montgomery form: false for infinity.
static bool to_affine(num x, num y, const point *p)
{
    if (is_zero(p->z)) return false;
    num zinv, t;
    inverse(zinv, p->z, &P);
    FMUL(t, p->x, zinv);
    from_mont(x, t, &P);
    FMUL(t, p->y, zinv);
    from_mont(y, t, &P);
    return true;
}

static void base_point(point *g)
{
    memcpy(g->x, GX, sizeof g->x);
    memcpy(g->y, GY, sizeof g->y);
    memcpy(g->z, P.one, sizeof g->z);
}

// A public key, which must be a point on the curve: y^2 = x^3 - 3x + b.
static bool decode_point(point *out, const uint8_t key[65])
{
    if (key[0] != 4) return false;
    num x, y;
    from_bytes(x, key + 1);
    from_bytes(y, key + 33);
    if (!less(x, P.m) || !less(y, P.m)) return false;
    to_mont(out->x, x, &P);
    to_mont(out->y, y, &P);
    memcpy(out->z, P.one, sizeof out->z);
    num lhs, rhs, t;
    FMUL(lhs, out->y, out->y);
    FMUL(rhs, out->x, out->x);
    FMUL(rhs, rhs, out->x);
    FADD(t, out->x, out->x);
    FADD(t, t, out->x);
    FSUB(rhs, rhs, t);
    FADD(rhs, rhs, B);
    return equal(lhs, rhs);
}

static void encode_point(uint8_t key[65], const num x, const num y)
{
    key[0] = 4;
    to_bytes(key + 1, x);
    to_bytes(key + 33, y);
}

// A scalar from 32 bytes: false unless it's 1 to n - 1.
static bool scalar(num k, const uint8_t in[32])
{
    from_bytes(k, in);
    return !is_zero(k) && less(k, N.m);
}

// ---------------------------------------------------------------------------

bool rtc_p256_public(const uint8_t private_key[32], uint8_t public_key[65])
{
    init();
    num d, x, y;
    if (!scalar(d, private_key)) return false;
    point g, q;
    base_point(&g);
    point_mul(&q, d, &g);
    if (!to_affine(x, y, &q)) return false;
    encode_point(public_key, x, y);
    return true;
}

bool rtc_p256_keys(uint8_t private_key[32], uint8_t public_key[65])
{
    init();
    num d;
    for (int tries = 0;; tries++) {
        if (tries == 64 || !rtc_random(private_key, 32)) return false;
        if (scalar(d, private_key)) break;
    }
    return rtc_p256_public(private_key, public_key);
}

bool rtc_p256_ecdh(const uint8_t private_key[32], const uint8_t their_key[65], uint8_t secret[32])
{
    init();
    num d, x, y;
    point q, s;
    if (!scalar(d, private_key) || !decode_point(&q, their_key)) return false;
    point_mul(&s, d, &q);
    if (!to_affine(x, y, &s)) return false;
    to_bytes(secret, x);
    return true;
}

// The hash as a number mod n: its 256 bits, less n if they're n or more.
static void hash_scalar(num e, const uint8_t hash[32])
{
    from_bytes(e, hash);
    reduce_once(e, e, &N);
}

// s = k^-1 (e + r d) mod n, all as plain numbers.
static void sign_with(num s, const num k, const num e, const num r, const num d)
{
    num km, kinv, rm, dm, em, t;
    to_mont(km, k, &N);
    inverse(kinv, km, &N);
    to_mont(rm, r, &N);
    to_mont(dm, d, &N);
    to_mont(em, e, &N);
    mont_mul(t, rm, dm, &N);
    add_mod(t, t, em, &N);
    mont_mul(t, kinv, t, &N);
    from_mont(s, t, &N);
}

bool rtc_p256_sign(const uint8_t private_key[32], const uint8_t hash[32], uint8_t sig[64])
{
    init();
    num d, e;
    if (!scalar(d, private_key)) return false;
    hash_scalar(e, hash);

    // RFC 6979, 3.2: k from HMAC-SHA256 of the key and the hash
    uint8_t h1[32], v[32], key[32];
    to_bytes(h1, e);
    memset(v, 1, sizeof v);
    memset(key, 0, sizeof key);
    for (uint8_t round = 0; round < 2; round++) {
        rtc_hmac256 h;
        rtc_hmac256_init(&h, key, 32);
        rtc_hmac256_add(&h, v, 32);
        rtc_hmac256_add(&h, &round, 1);
        rtc_hmac256_add(&h, private_key, 32);
        rtc_hmac256_add(&h, h1, 32);
        rtc_hmac256_end(&h, key);
        rtc_hmac256_init(&h, key, 32);
        rtc_hmac256_add(&h, v, 32);
        rtc_hmac256_end(&h, v);
    }
    for (int tries = 0; tries < 64; tries++) {
        rtc_hmac256 h;
        rtc_hmac256_init(&h, key, 32);
        rtc_hmac256_add(&h, v, 32);
        rtc_hmac256_end(&h, v);
        num k;
        if (scalar(k, v)) {
            point g, kg;
            base_point(&g);
            point_mul(&kg, k, &g);
            num x, y, r, s;
            if (to_affine(x, y, &kg)) {
                reduce_once(r, x, &N);
                sign_with(s, k, e, r, d);
                if (!is_zero(r) && !is_zero(s)) {
                    to_bytes(sig, r);
                    to_bytes(sig + 32, s);
                    return true;
                }
            }
        }
        const uint8_t zero = 0;
        rtc_hmac256_init(&h, key, 32);
        rtc_hmac256_add(&h, v, 32);
        rtc_hmac256_add(&h, &zero, 1);
        rtc_hmac256_end(&h, key);
        rtc_hmac256_init(&h, key, 32);
        rtc_hmac256_add(&h, v, 32);
        rtc_hmac256_end(&h, v);
    }
    return false;
}

bool rtc_p256_verify(const uint8_t public_key[65], const uint8_t hash[32], const uint8_t sig[64])
{
    init();
    num r, s, e;
    point q;
    if (!scalar(r, sig) || !scalar(s, sig + 32) || !decode_point(&q, public_key)) return false;
    hash_scalar(e, hash);
    // u1 = e / s, u2 = r / s; then u1 G + u2 Q must have x = r (mod n)
    num sm, winv, em, rm, u1, u2;
    to_mont(sm, s, &N);
    inverse(winv, sm, &N);
    to_mont(em, e, &N);
    to_mont(rm, r, &N);
    mont_mul(u1, em, winv, &N);
    mont_mul(u2, rm, winv, &N);
    from_mont(u1, u1, &N);
    from_mont(u2, u2, &N);
    point g, a, b;
    base_point(&g);
    point_mul(&a, u1, &g);
    point_mul(&b, u2, &q);
    point_add(&a, &a, &b);
    num x, y, v;
    if (!to_affine(x, y, &a)) return false;
    reduce_once(v, x, &N);
    return equal(v, r);
}
