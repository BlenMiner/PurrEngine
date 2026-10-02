#include "tide/text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// See tide/text.h.

// ---------------------------------------------------------------------------
// The scratch area: a stack of bytes, one per thread, allocated on first use.

TIDE_THREAD_LOCAL char *tide_scratch_area;
static TIDE_THREAD_LOCAL uint32_t scratch_used;

uint32_t tide_scratch_mark(void)
{
    return scratch_used;
}

void tide_scratch_reset(const uint32_t mark)
{
    if (mark < scratch_used) scratch_used = mark;
}

void tide_scratch_free(void)
{
    free(tide_scratch_area);
    tide_scratch_area = NULL;
    scratch_used = 0;
}

// Room for `bytes` bytes and a NUL, or NULL when the area is full.
static char *scratch_alloc(const uint32_t bytes)
{
    if (!tide_scratch_area) {
        tide_scratch_area = malloc(TIDE_SCRATCH_BYTES);
        if (!tide_scratch_area) return NULL;
        tide_memory_sync(); // Before anything touches it (see tide/page.h)
    }
    if (bytes >= TIDE_SCRATCH_BYTES - scratch_used) return NULL;
    char *p = tide_scratch_area + scratch_used;
    scratch_used += bytes + 1;
    p[bytes] = '\0';
    return p;
}

// Whether `a` is the newest text in the scratch area, so it can grow in place.
static bool at_top(const tide_str a)
{
    if (!tide_scratch_area) return false;
    const uintptr_t start = (uintptr_t)tide_scratch_area;
    const uintptr_t p = (uintptr_t)a.ptr;
    return p >= start && p + (uintptr_t)a.bytes + 1u == start + scratch_used;
}

// Makes room for `more` bytes after `a`'s, in place or in a copy, and returns
// where they go, or NULL when the area is full (then `a` is unchanged).
static char *grow(tide_str *a, const uint32_t more)
{
    if (at_top(*a)) {
        if (more >= TIDE_SCRATCH_BYTES - scratch_used) return NULL;
        scratch_used += more;
        char *p = (char *)(uintptr_t)a->ptr;
        p[(uint32_t)a->bytes + more] = '\0';
        return p + a->bytes;
    }
    char *p = scratch_alloc((uint32_t)a->bytes + more);
    if (!p) return NULL;
    memcpy(p, a->ptr, (size_t)a->bytes);
    a->ptr = p;
    return p + a->bytes;
}

// `a` followed by `count` bytes holding `chars` characters. When the area is
// full, `a` stays as it is.
static tide_str append(tide_str a, const char *bytes, const int32_t count, const int32_t chars)
{
    if (count <= 0) return a;
    char *to = grow(&a, (uint32_t)count);
    if (!to) return a;
    memmove(to, bytes, (size_t)count);
    a.bytes += count;
    a.chars += chars;
    return a;
}

// A copy of `count` bytes in the scratch area.
static tide_str copy(const char *bytes, const int32_t count)
{
    return append(TIDE_STR_EMPTY, bytes, count, tide_utf8_chars(bytes, count));
}

int32_t tide_utf8_chars(const char *bytes, const int32_t count)
{
    int32_t chars = 0;
    for (int32_t i = 0; i < count; i++) chars += ((unsigned char)bytes[i] & 0xC0u) != 0x80u;
    return chars;
}

tide_str tide_str_from_cstr(const char *s)
{
    const int32_t bytes = (int32_t)strlen(s);
    return (tide_str){s, bytes, tide_utf8_chars(s, bytes)};
}

const char *tide_str_c(const tide_str s)
{
    return s.ptr[s.bytes] == '\0' ? s.ptr : copy(s.ptr, s.bytes).ptr;
}

tide_str tide_str_copy_cstr(const char *s)
{
    return s ? copy(s, (int32_t)strlen(s)) : TIDE_STR_EMPTY;
}

tide_str tide_str_add(const tide_str a, const tide_str b)
{
    return append(a, b.ptr, b.bytes, b.chars);
}

tide_str tide_str_add_cstr(const tide_str a, const char *s)
{
    return tide_str_add(a, tide_str_from_cstr(s));
}

static tide_str add_ascii(const tide_str a, const char *s, const int32_t count)
{
    return append(a, s, count, count);
}

tide_str tide_str_add_bool(const tide_str a, const bool v)
{
    return v ? add_ascii(a, "true", 4) : add_ascii(a, "false", 5);
}

// ---------------------------------------------------------------------------
// Big integers, just enough to write floats exactly: a float is f * 2^e, and
// the digits of that are found by exact integer arithmetic, not float math.

#define BIG_WORDS 16

typedef struct big {
    int n;               // Words in use; the top one is nonzero
    uint32_t w[BIG_WORDS]; // Least significant first
} big;

static void big_set(big *b, const uint32_t v)
{
    b->w[0] = v;
    b->n = v != 0;
}

static void big_mul_small(big *b, const uint32_t m)
{
    uint64_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        const uint64_t t = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && b->n < BIG_WORDS) b->w[b->n++] = (uint32_t)carry;
}

static void big_mul_pow10(big *b, int k)
{
    static const uint32_t powers[] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u};
    for (; k >= 9; k -= 9) big_mul_small(b, 1000000000u);
    if (k > 0) big_mul_small(b, powers[k]);
}

static void big_shl(big *b, const int bits)
{
    if (b->n == 0 || bits <= 0) return;
    const int words = bits / 32;
    const int rest = bits % 32;
    int n = b->n + words + 1;
    if (n > BIG_WORDS) n = BIG_WORDS;
    for (int i = n - 1; i >= 0; i--) {
        const int from = i - words;
        const uint32_t high = from >= 0 && from < b->n ? b->w[from] : 0u;
        const uint32_t low = from - 1 >= 0 && from - 1 < b->n ? b->w[from - 1] : 0u;
        b->w[i] = rest ? (high << rest) | (low >> (32 - rest)) : high;
    }
    b->n = n;
    while (b->n > 0 && b->w[b->n - 1] == 0) b->n--;
}

static void big_shr(big *b, const int bits)
{
    const int words = bits / 32;
    const int rest = bits % 32;
    for (int i = 0; i < b->n; i++) {
        const int from = i + words;
        const uint32_t low = from < b->n ? b->w[from] : 0u;
        const uint32_t high = from + 1 < b->n ? b->w[from + 1] : 0u;
        b->w[i] = rest ? (low >> rest) | (high << (32 - rest)) : low;
    }
    b->n = b->n > words ? b->n - words : 0;
    while (b->n > 0 && b->w[b->n - 1] == 0) b->n--;
}

static bool big_bit(const big *b, const int i)
{
    return i / 32 < b->n && (b->w[i / 32] >> (i % 32) & 1u);
}

static int big_cmp(const big *a, const big *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    }
    return 0;
}

static void big_add(big *a, const big *b)
{
    uint64_t carry = 0;
    const int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        const uint64_t t = (uint64_t)(i < a->n ? a->w[i] : 0u) + (i < b->n ? b->w[i] : 0u) + carry;
        a->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    a->n = n;
    if (carry && a->n < BIG_WORDS) a->w[a->n++] = (uint32_t)carry;
}

// a -= b, where a >= b.
static void big_sub(big *a, const big *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        const int64_t t = (int64_t)a->w[i] - (i < b->n ? b->w[i] : 0u) - borrow;
        a->w[i] = (uint32_t)t;
        borrow = t < 0;
    }
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

// The decimal digits of `b`, most significant first. Returns how many.
static int big_digits(big b, char *out)
{
    char reversed[160];
    int n = 0;
    do {
        uint64_t rest = 0;
        for (int i = b.n - 1; i >= 0; i--) {
            const uint64_t t = rest << 32 | b.w[i];
            b.w[i] = (uint32_t)(t / 10u);
            rest = t % 10u;
        }
        while (b.n > 0 && b.w[b.n - 1] == 0) b.n--;
        reversed[n++] = (char)('0' + rest);
    } while (b.n > 0);
    for (int i = 0; i < n; i++) out[i] = reversed[n - 1 - i];
    return n;
}

// ---------------------------------------------------------------------------
// Numbers to text

// A float's parts: v = f * 2^e, f an integer.
typedef struct float_parts {
    bool negative;
    bool finite;
    bool nan;
    uint32_t f;
    int e;
    bool denormal;
} float_parts;

static float_parts split_float(const float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    const uint32_t exponent = bits >> 23 & 0xFFu;
    const uint32_t fraction = bits & 0x7FFFFFu;
    float_parts p = {bits >> 31 != 0, exponent != 0xFFu, exponent == 0xFFu && fraction != 0, 0, 0, exponent == 0};
    if (exponent == 0) {
        p.f = fraction;
        p.e = -149;
    } else {
        p.f = fraction | 0x800000u;
        p.e = (int)exponent - 150;
    }
    return p;
}

// `v` with `decimals` digits after the point, the last one rounded half away
// from zero, from its exact value. Returns the length written to `out`.
static int write_fixed(char *out, const float_parts *p, const int decimals)
{
    big n;
    big_set(&n, p->f);
    if (p->e > 0) big_shl(&n, p->e);
    big_mul_pow10(&n, decimals);
    const int shift = p->e < 0 ? -p->e : 0;
    const bool up = shift > 0 && big_bit(&n, shift - 1); // The part cut off is at least half
    big_shr(&n, shift);
    if (up) {
        big one;
        big_set(&one, 1);
        big_add(&n, &one);
    }
    char digits[160];
    const int count = big_digits(n, digits);
    const bool zero = count == 1 && digits[0] == '0';
    int len = 0;
    if (p->negative && !zero) out[len++] = '-';
    if (count <= decimals) {
        out[len++] = '0';
        out[len++] = '.';
        for (int i = count; i < decimals; i++) out[len++] = '0';
        memcpy(out + len, digits, (size_t)count);
        len += count;
    } else {
        memcpy(out + len, digits, (size_t)(count - decimals));
        len += count - decimals;
        if (decimals > 0) {
            out[len++] = '.';
            memcpy(out + len, digits + count - decimals, (size_t)decimals);
            len += decimals;
        }
    }
    return len;
}

// The fewest digits that read back as exactly `v`: Burger and Dybvig's
// free-format algorithm, "Printing Floating-Point Numbers Quickly and
// Accurately" (1996). Writes the digits and returns how many; `*point` is
// where the decimal point goes: v = 0.d1d2... * 10^point.
static int shortest_digits(const float_parts *p, char *digits, int *point)
{
    // The numbers that round to v are those between the midpoints to its
    // neighbours, (r - m-)/s and (r + m+)/s, and include them when f is even,
    // as IEEE rounding goes to even. Above a power of two, the gap below is
    // half the gap above.
    const bool even = (p->f & 1u) == 0;
    const bool boundary = p->f == 0x800000u && p->e > -149;
    big r, s, up, down;
    if (p->e >= 0) {
        big_set(&r, p->f);
        big_shl(&r, p->e + (boundary ? 2 : 1));
        big_set(&s, boundary ? 4u : 2u);
        big_set(&up, 1);
        big_shl(&up, p->e + (boundary ? 1 : 0));
        big_set(&down, 1);
        big_shl(&down, p->e);
    } else {
        big_set(&r, p->f);
        big_shl(&r, boundary ? 2 : 1);
        big_set(&s, 1);
        big_shl(&s, -p->e + (boundary ? 2 : 1));
        big_set(&up, boundary ? 2u : 1u);
        big_set(&down, 1);
    }

    // Scale by a power of ten so the first digit is the one after the point.
    int k = 0;
    for (;;) {
        big high = r;
        big_add(&high, &up);
        const int c = big_cmp(&high, &s);
        if (even ? c < 0 : c <= 0) break;
        big_mul_small(&s, 10);
        k++;
    }
    for (;;) {
        big high = r;
        big_add(&high, &up);
        big_mul_small(&high, 10);
        const int c = big_cmp(&high, &s);
        if (even ? c >= 0 : c > 0) break;
        big_mul_small(&r, 10);
        big_mul_small(&up, 10);
        big_mul_small(&down, 10);
        k--;
    }

    int n = 0;
    for (;;) {
        big_mul_small(&r, 10);
        big_mul_small(&up, 10);
        big_mul_small(&down, 10);
        int d = 0;
        while (big_cmp(&r, &s) >= 0) {
            big_sub(&r, &s);
            d++;
        }
        const int low_c = big_cmp(&r, &down);
        const bool low = even ? low_c <= 0 : low_c < 0;
        big high = r;
        big_add(&high, &up);
        const int high_c = big_cmp(&high, &s);
        const bool high_ok = even ? high_c >= 0 : high_c > 0;
        if (!low && !high_ok && n < 16) {
            digits[n++] = (char)('0' + d);
            continue;
        }
        if (low && !high_ok) {
            digits[n++] = (char)('0' + d);
        } else if (high_ok && !low) {
            digits[n++] = (char)('0' + d + 1);
        } else {
            // Both would read back as v: the closer one, and on a tie the even one.
            big twice = r;
            big_mul_small(&twice, 2);
            const int c = big_cmp(&twice, &s);
            digits[n++] = (char)('0' + (c < 0 || (c == 0 && d % 2 == 0) ? d : d + 1));
        }
        break;
    }
    *point = k;
    return n;
}

// The shortest text that reads back as `v`: plain from 1e-7 up to 1e21, like
// JavaScript, and with an exponent beyond ("1.5E+21").
static int write_shortest(char *out, const float_parts *p)
{
    int len = 0;
    if (p->f == 0) {
        out[len++] = '0';
        return len;
    }
    char digits[32];
    int point;
    const int n = shortest_digits(p, digits, &point);
    if (p->negative) out[len++] = '-';
    const int exponent = point - 1;
    if (exponent >= 21 || exponent <= -7) {
        out[len++] = digits[0];
        if (n > 1) {
            out[len++] = '.';
            memcpy(out + len, digits + 1, (size_t)(n - 1));
            len += n - 1;
        }
        out[len++] = 'E';
        out[len++] = exponent < 0 ? '-' : '+';
        char e[8];
        const int e_len = big_digits((big){1, {(uint32_t)(exponent < 0 ? -exponent : exponent)}}, e);
        memcpy(out + len, e, (size_t)e_len);
        return len + e_len;
    }
    if (point <= 0) {
        out[len++] = '0';
        out[len++] = '.';
        for (int i = point; i < 0; i++) out[len++] = '0';
        memcpy(out + len, digits, (size_t)n);
        return len + n;
    }
    if (point >= n) {
        memcpy(out + len, digits, (size_t)n);
        len += n;
        for (int i = n; i < point; i++) out[len++] = '0';
        return len;
    }
    memcpy(out + len, digits, (size_t)point);
    len += point;
    out[len++] = '.';
    memcpy(out + len, digits + point, (size_t)(n - point));
    return len + n - point;
}

static int write_float(char *out, const float v, const int32_t format)
{
    const float_parts p = split_float(v);
    if (p.nan) {
        memcpy(out, "NaN", 3);
        return 3;
    }
    if (!p.finite) {
        memcpy(out, p.negative ? "-Infinity" : "Infinity", p.negative ? 9 : 8);
        return p.negative ? 9 : 8;
    }
    const char letter = (char)(format >> 8);
    if (letter == 'F' || letter == 'f') return write_fixed(out, &p, format & 0xFF);
    return write_shortest(out, &p);
}

// An int in decimal (D: at least `digits` digits), hex (X: upper case, x:
// lower, two's complement for negatives), or with F's decimals, all zeros.
static int write_int(char *out, const int32_t v, const int32_t format)
{
    const char letter = (char)(format >> 8);
    const int digits = format & 0xFF;
    int len = 0;
    char buf[16];
    int n = 0;
    if (letter == 'X' || letter == 'x') {
        const char *hex = letter == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        uint32_t u = (uint32_t)v;
        do {
            buf[n++] = hex[u & 15u];
            u >>= 4;
        } while (u);
        for (int i = n; i < digits; i++) out[len++] = '0';
        for (int i = n - 1; i >= 0; i--) out[len++] = buf[i];
        return len;
    }
    // Negated as unsigned, so the lowest int works too.
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    do {
        buf[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    if (v < 0) out[len++] = '-';
    if (letter == 'D' || letter == 'd') {
        for (int i = n; i < digits; i++) out[len++] = '0';
    }
    for (int i = n - 1; i >= 0; i--) out[len++] = buf[i];
    if ((letter == 'F' || letter == 'f') && digits > 0) {
        out[len++] = '.';
        for (int i = 0; i < digits; i++) out[len++] = '0';
    }
    return len;
}

tide_str tide_str_add_int(const tide_str a, const int32_t v, const int32_t format)
{
    char buf[64];
    return add_ascii(a, buf, write_int(buf, v, format));
}

tide_str tide_str_add_float(const tide_str a, const float v, const int32_t format)
{
    char buf[128];
    return add_ascii(a, buf, write_float(buf, v, format));
}

tide_str tide_str_add_entity(tide_str a, const tide_entity e, const bool local)
{
    a = add_ascii(a, local ? "LocalEntity(" : "Entity(", local ? 12 : 7);
    if (tide_entity_is_null(e)) return add_ascii(a, "none)", 5);
    if (tide_entity_is_temporary(e)) return add_ascii(a, "new)", 4); // Its ID comes once its system is done
    a = tide_str_add_int(a, (int32_t)e.index, 0);
    a = add_ascii(a, ":", 1);
    a = tide_str_add_int(a, (int32_t)e.generation, 0);
    return add_ascii(a, ")", 1);
}

tide_str tide_str_add_player(tide_str a, const tide_player_id p)
{
    a = add_ascii(a, "PlayerID(", 9);
    if (tide_player_is_null(p)) return add_ascii(a, "none)", 5);
    a = tide_str_add_int(a, (int32_t)(p.id - 1u), 0);
    return add_ascii(a, ")", 1);
}

// "(1, 2, 3)", each number written with `format`.
static tide_str add_floats(tide_str a, const char *open, const float *v, const int n, const int32_t format)
{
    a = tide_str_add_cstr(a, open);
    for (int i = 0; i < n; i++) {
        if (i) a = add_ascii(a, ", ", 2);
        a = tide_str_add_float(a, v[i], format);
    }
    return add_ascii(a, ")", 1);
}

static tide_str add_ints(tide_str a, const int32_t *v, const int n, const int32_t format)
{
    a = add_ascii(a, "(", 1);
    for (int i = 0; i < n; i++) {
        if (i) a = add_ascii(a, ", ", 2);
        a = tide_str_add_int(a, v[i], format);
    }
    return add_ascii(a, ")", 1);
}

tide_str tide_str_add_i2(const tide_str a, const tide_int2 v, const int32_t format)
{
    const int32_t c[2] = {v.x, v.y};
    return add_ints(a, c, 2, format);
}

tide_str tide_str_add_i3(const tide_str a, const tide_int3 v, const int32_t format)
{
    const int32_t c[3] = {v.x, v.y, v.z};
    return add_ints(a, c, 3, format);
}

tide_str tide_str_add_i4(const tide_str a, const tide_int4 v, const int32_t format)
{
    const int32_t c[4] = {v.x, v.y, v.z, v.w};
    return add_ints(a, c, 4, format);
}

tide_str tide_str_add_f2(const tide_str a, const tide_float2 v, const int32_t format)
{
    const float c[2] = {v.x, v.y};
    return add_floats(a, "(", c, 2, format);
}

tide_str tide_str_add_f3(const tide_str a, const tide_float3 v, const int32_t format)
{
    const float c[3] = {v.x, v.y, v.z};
    return add_floats(a, "(", c, 3, format);
}

tide_str tide_str_add_f4(const tide_str a, const tide_float4 v, const int32_t format)
{
    const float c[4] = {v.x, v.y, v.z, v.w};
    return add_floats(a, "(", c, 4, format);
}

tide_str tide_str_add_q(const tide_str a, const tide_quaternion v, const int32_t format)
{
    const float c[4] = {v.value.x, v.value.y, v.value.z, v.value.w};
    return add_floats(a, "(", c, 4, format);
}

tide_str tide_str_add_color(const tide_str a, const tide_color v, const int32_t format)
{
    const float c[4] = {v.r, v.g, v.b, v.a};
    return add_floats(a, "RGBA(", c, 4, format);
}

tide_str tide_str_add_rect(tide_str a, const tide_rect v, const int32_t format)
{
    a = add_ascii(a, "(x:", 3);
    a = tide_str_add_float(a, v.x, format);
    a = add_ascii(a, ", y:", 4);
    a = tide_str_add_float(a, v.y, format);
    a = add_ascii(a, ", width:", 8);
    a = tide_str_add_float(a, v.width, format);
    a = add_ascii(a, ", height:", 9);
    a = tide_str_add_float(a, v.height, format);
    return add_ascii(a, ")", 1);
}

// ---------------------------------------------------------------------------
// Comparing and searching

bool tide_str_eq(const tide_str a, const tide_str b)
{
    return a.bytes == b.bytes && memcmp(a.ptr, b.ptr, (size_t)a.bytes) == 0;
}

// The byte where `b` first appears in `a`, or -1.
static int32_t find(const tide_str a, const tide_str b)
{
    if (b.bytes == 0) return 0;
    for (int32_t i = 0; i + b.bytes <= a.bytes; i++) {
        if (memcmp(a.ptr + i, b.ptr, (size_t)b.bytes) == 0) return i;
    }
    return -1;
}

bool tide_str_contains(const tide_str a, const tide_str b)
{
    return find(a, b) >= 0;
}

bool tide_str_starts_with(const tide_str a, const tide_str b)
{
    return b.bytes <= a.bytes && memcmp(a.ptr, b.ptr, (size_t)b.bytes) == 0;
}

bool tide_str_ends_with(const tide_str a, const tide_str b)
{
    return b.bytes <= a.bytes && memcmp(a.ptr + a.bytes - b.bytes, b.ptr, (size_t)b.bytes) == 0;
}

int32_t tide_str_index_of(const tide_str a, const tide_str b)
{
    const int32_t at = find(a, b);
    return at < 0 ? -1 : tide_utf8_chars(a.ptr, at);
}

// The byte where character `index` starts, or a.bytes past the end.
static int32_t byte_of_char(const tide_str a, const int32_t index)
{
    int32_t chars = 0;
    for (int32_t i = 0; i < a.bytes; i++) {
        if (((unsigned char)a.ptr[i] & 0xC0u) != 0x80u) {
            if (chars == index) return i;
            chars++;
        }
    }
    return a.bytes;
}

tide_str tide_str_substring(const tide_str a, int32_t start, int32_t length)
{
    if (start < 0) start = 0;
    if (start > a.chars) start = a.chars;
    if (length < 0) length = 0;
    if (length > a.chars - start) length = a.chars - start;
    const int32_t from = byte_of_char(a, start);
    const int32_t to = byte_of_char(a, start + length);
    if (to == a.bytes) return (tide_str){a.ptr + from, a.bytes - from, a.chars - start}; // Ends with a's NUL
    return copy(a.ptr + from, to - from);
}

tide_str tide_str_substring_from(const tide_str a, const int32_t start)
{
    return tide_str_substring(a, start, a.chars);
}

static tide_str map_ascii(const tide_str a, const bool upper)
{
    tide_str out = copy(a.ptr, a.bytes);
    if (out.bytes != a.bytes) return a; // The area is full: unchanged
    char *p = (char *)(uintptr_t)out.ptr;
    for (int32_t i = 0; i < out.bytes; i++) {
        if (upper && p[i] >= 'a' && p[i] <= 'z') p[i] = (char)(p[i] - 'a' + 'A');
        if (!upper && p[i] >= 'A' && p[i] <= 'Z') p[i] = (char)(p[i] - 'A' + 'a');
    }
    return out;
}

tide_str tide_str_to_upper(const tide_str a)
{
    return map_ascii(a, true);
}

tide_str tide_str_to_lower(const tide_str a)
{
    return map_ascii(a, false);
}

static bool is_space(const char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

tide_str tide_str_trim(const tide_str a)
{
    int32_t from = 0;
    int32_t to = a.bytes;
    while (from < to && is_space(a.ptr[from])) from++;
    while (to > from && is_space(a.ptr[to - 1])) to--;
    if (to == a.bytes) return (tide_str){a.ptr + from, a.bytes - from, a.chars - from}; // Spaces are one byte each
    return copy(a.ptr + from, to - from);
}

tide_str tide_str_replace(const tide_str a, const tide_str from, const tide_str to)
{
    if (from.bytes == 0 || find(a, from) < 0) return a;
    tide_str out = TIDE_STR_EMPTY;
    int32_t i = 0;
    while (i < a.bytes) {
        if (i + from.bytes <= a.bytes && memcmp(a.ptr + i, from.ptr, (size_t)from.bytes) == 0) {
            out = tide_str_add(out, to);
            i += from.bytes;
            continue;
        }
        int32_t run = i + 1; // Up to the next place `from` could start
        while (run < a.bytes && a.ptr[run] != from.ptr[0]) run++;
        out = append(out, a.ptr + i, run - i, tide_utf8_chars(a.ptr + i, run - i));
        i = run;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Text in fields

#define TEXT_WHERE(at) ((at) >> 30)
#define TEXT_OFFSET(at) ((at) & 0x3FFFFFFFu)

TIDE_THREAD_LOCAL tide_heap *tide_world_heaps[2];

void tide_text_use(tide_heap *match_heap, tide_heap *local_heap)
{
    tide_world_heaps[TIDE_IN_MATCH] = match_heap;
    tide_world_heaps[TIDE_IN_LOCAL] = local_heap;
}

static tide_str view_block(const tide_heap *heap, const uint32_t offset)
{
    const tide_block *b = tide_heap_block(heap, offset);
    return (tide_str){(const char *)(b + 1), (int32_t)b->a, (int32_t)b->b};
}

tide_str tide_text_read(const tide_heap *heap, const tide_text t)
{
    return t.at ? view_block(heap, TEXT_OFFSET(t.at)) : TIDE_STR_EMPTY;
}

tide_str tide_text_view(const tide_text t)
{
    if (!t.at) return TIDE_STR_EMPTY;
    const uint32_t where = TEXT_WHERE(t.at);
    const uint32_t offset = TEXT_OFFSET(t.at);
    if (where == TIDE_IN_SCRATCH) {
        const tide_block *b = (const tide_block *)(uintptr_t)(tide_scratch_area + offset);
        return (tide_str){(const char *)(b + 1), (int32_t)b->a, (int32_t)b->b};
    }
    const tide_heap *heap = tide_heap_of(where);
    return heap ? view_block(heap, offset) : TIDE_STR_EMPTY;
}

tide_text tide_text_temp(const tide_str value)
{
    if (value.bytes == 0) return (tide_text){0};
    // A header, like a heap block's, then the text and a NUL. Scratch
    // offsets are kept 4-aligned for the header.
    const uint32_t pad = (4u - scratch_used % 4u) % 4u;
    char *p = scratch_alloc(pad + (uint32_t)sizeof(tide_block) + (uint32_t)value.bytes);
    if (!p) return (tide_text){0};
    tide_block *b = (tide_block *)(uintptr_t)(p + pad);
    *b = (tide_block){0, 0, (uint32_t)value.bytes, (uint32_t)value.chars};
    memcpy(b + 1, value.ptr, (size_t)value.bytes);
    return (tide_text){TIDE_IN_SCRATCH << 30 | (uint32_t)((char *)b - tide_scratch_area)};
}

// A block in `heap` holding `value`, tagged as `where`'s.
static uint32_t heap_copy(tide_heap *heap, const uint32_t where, const tide_str value)
{
    const uint32_t block = tide_heap_alloc(heap, (uint32_t)value.bytes + 1u);
    tide_block *b = tide_heap_write(heap, block);
    b->a = (uint32_t)value.bytes;
    b->b = (uint32_t)value.chars;
    char *bytes = (char *)(b + 1);
    memcpy(bytes, value.ptr, (size_t)value.bytes);
    bytes[value.bytes] = '\0';
    return where << 30 | block;
}

void tide_text_set(tide_text *field, const tide_str value, const uint32_t where)
{
    tide_heap *heap = tide_heap_of(where);
    if (!heap) {
        *field = tide_text_temp(value);
        return;
    }
    // The new text first: `value` may be the old text itself.
    const uint32_t at = value.bytes > 0 ? heap_copy(heap, where, value) : 0u;
    tide_text_release(field, where);
    field->at = at;
}

void tide_text_own(tide_text *field, const uint32_t where)
{
    tide_heap *heap = tide_heap_of(where);
    if (!heap || !field->at) return;
    field->at = heap_copy(heap, where, tide_text_view(*field));
}

void tide_text_release(tide_text *field, const uint32_t where)
{
    tide_heap *heap = tide_heap_of(where);
    if (!heap || !field->at || TEXT_WHERE(field->at) != where) return;
    tide_heap_release(heap, TEXT_OFFSET(field->at));
    field->at = 0;
}

// ---------------------------------------------------------------------------
// For tide/list.h

tide_block *tide_scratch_block(const uint32_t bytes, uint32_t *at)
{
    const uint32_t pad = (16u - scratch_used % 16u) % 16u;
    char *p = scratch_alloc(pad + (uint32_t)sizeof(tide_block) + bytes);
    if (!p) return NULL;
    tide_block *b = (tide_block *)(uintptr_t)(p + pad);
    *b = (tide_block){0, 0, 0, 0};
    *at = TIDE_IN_SCRATCH << 30 | (uint32_t)((char *)b - tide_scratch_area);
    return b;
}
