#include "release.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

typedef struct version {
    unsigned long long core[3]; // Major, minor, patch
    const char *pre;            // The pre-release after '-', or NULL for a release
    size_t pre_len;
} version;

// MAJOR.MINOR.PATCH, then an optional -PRERELEASE and +BUILD (which doesn't count).
static bool parse(const char *s, version *v)
{
    if (!s) return false;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        unsigned long long n = 0;
        while (isdigit((unsigned char)*s)) n = n * 10u + (unsigned long long)(*s++ - '0');
        v->core[i] = n;
        if (i < 2 && *s++ != '.') return false;
    }
    v->pre = NULL;
    v->pre_len = 0;
    if (*s == '-') {
        v->pre = ++s;
        while (*s && *s != '+') s++;
        v->pre_len = (size_t)(s - v->pre);
        if (v->pre_len == 0) return false;
    }
    return *s == '\0' || *s == '+';
}

static bool all_digits(const char *s, const size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return n > 0;
}

// One identifier of a pre-release against another: numbers by value, and
// before words, which compare as ASCII.
static int compare_identifier(const char *a, size_t an, const char *b, size_t bn)
{
    const bool a_number = all_digits(a, an);
    const bool b_number = all_digits(b, bn);
    if (a_number != b_number) return a_number ? -1 : 1;
    if (a_number) {
        while (an > 1 && *a == '0') {
            a++;
            an--;
        }
        while (bn > 1 && *b == '0') {
            b++;
            bn--;
        }
        if (an != bn) return an < bn ? -1 : 1;
    }
    const int c = memcmp(a, b, an < bn ? an : bn);
    if (c != 0) return c < 0 ? -1 : 1;
    return an == bn ? 0 : an < bn ? -1 : 1;
}

// Dot-separated identifiers, one by one; when one side runs out first, it's older.
static int compare_prerelease(const char *a, const size_t an, const char *b, const size_t bn)
{
    size_t i = 0;
    size_t j = 0;
    while (i <= an && j <= bn) {
        size_t i_end = i;
        size_t j_end = j;
        while (i_end < an && a[i_end] != '.') i_end++;
        while (j_end < bn && b[j_end] != '.') j_end++;
        const int c = compare_identifier(a + i, i_end - i, b + j, j_end - j);
        if (c != 0) return c;
        i = i_end + 1;
        j = j_end + 1;
    }
    return (i <= an) - (j <= bn);
}

int purr_version_compare(const char *a, const char *b)
{
    version x;
    version y;
    const bool x_valid = parse(a, &x);
    const bool y_valid = parse(b, &y);
    if (!x_valid || !y_valid) return (int)x_valid - (int)y_valid;
    for (int i = 0; i < 3; i++) {
        if (x.core[i] != y.core[i]) return x.core[i] < y.core[i] ? -1 : 1;
    }
    if (!x.pre || !y.pre) return (x.pre == NULL) - (y.pre == NULL);
    return compare_prerelease(x.pre, x.pre_len, y.pre, y.pre_len);
}

bool purr_version_valid(const char *text)
{
    version v;
    return parse(text, &v);
}

const char *purr_release_version(const json *release)
{
    const char *tag = json_str(json_get(release, "tag_name"));
    return tag && tag[0] == 'v' ? tag + 1 : tag;
}

static bool is_true(const json *v)
{
    return v && v->kind == JSON_TRUE;
}

static bool allowed(const json *release, const char *channel)
{
    version v;
    if (!parse(purr_release_version(release), &v) || is_true(json_get(release, "draft"))) return false;
    return strcmp(channel, "stable") != 0 || !is_true(json_get(release, "prerelease"));
}

const json *purr_release_pick(const json *releases, const json *latest, const char *channel)
{
    const json *best = latest && allowed(latest, channel) ? latest : NULL;
    for (int i = 0; releases && releases->kind == JSON_ARRAY && i < releases->count; i++) {
        const json *r = releases->items[i];
        if (!allowed(r, channel)) continue;
        if (!best || purr_version_compare(purr_release_version(r), purr_release_version(best)) > 0) best = r;
    }
    return best;
}

bool purr_release_is(const json *release, const char *version)
{
    if (version && version[0] == 'v') version++;
    const char *v = purr_release_version(release);
    return v && version && strcmp(v, version) == 0 && !is_true(json_get(release, "draft"));
}

const json *purr_release_find(const json *releases, const char *version)
{
    for (int i = 0; releases && releases->kind == JSON_ARRAY && i < releases->count; i++) {
        if (purr_release_is(releases->items[i], version)) return releases->items[i];
    }
    return NULL;
}
