#ifndef _WIN32
#define _DEFAULT_SOURCE // stat under strict C
#endif

#include "packages.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static char *dup_n(const char *s, const size_t n)
{
    char *out = malloc(n + 1);
    if (!out) abort();
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *dup(const char *s)
{
    return dup_n(s, strlen(s));
}

static char *format(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    char *out = malloc((size_t)(n > 0 ? n : 0) + 1);
    if (!out) abort();
    va_start(args, fmt);
    vsnprintf(out, (size_t)(n > 0 ? n : 0) + 1, fmt, args);
    va_end(args);
    return out;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)(size > 0 ? size : 0) + 1);
    if (!text) abort();
    const size_t n = fread(text, 1, (size_t)(size > 0 ? size : 0), f);
    fclose(f);
    text[n] = '\0';
    return text;
}

// ---------------------------------------------------------------------------
// Paths, with forward slashes

static bool is_separator(const char c)
{
    return c == '/' || c == '\\';
}

static bool is_absolute(const char *path)
{
    return is_separator(path[0]) || (isalpha((unsigned char)path[0]) && path[1] == ':');
}

// `path` with forward slashes and its "." and ".." parts worked out, ending
// in '/'. malloc'd.
static char *normalize_folder(const char *path)
{
    const size_t n = strlen(path);
    char *out = malloc(n * 2 + 4); // Each part gains a '/', and a last ".." is "../"
    if (!out) abort();
    size_t len = 0;
    size_t root = 0; // What ".." can't go above: "/" or "D:/"
    const char *p = path;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        out[len++] = p[0];
        out[len++] = ':';
        p += 2;
    }
    if (is_separator(*p)) {
        out[len++] = '/';
        p++;
    }
    root = len;
    while (*p) {
        const char *end = p;
        while (*end && !is_separator(*end)) end++;
        const size_t part = (size_t)(end - p);
        if (part == 2 && p[0] == '.' && p[1] == '.') {
            // Back over the last part, unless there's none left to go back over
            size_t back = len;
            if (back > root) back--; // Its '/'
            while (back > root && out[back - 1] != '/') back--;
            const bool up = len > root && !(len - back == 3 && memcmp(out + back, "../", 3) == 0);
            if (up) {
                len = back;
            } else if (root == 0) { // A relative path keeps going up
                memcpy(out + len, "../", 3);
                len += 3;
            }
        } else if (part > 0 && !(part == 1 && p[0] == '.')) {
            memcpy(out + len, p, part);
            len += part;
            out[len++] = '/';
        }
        p = *end ? end + 1 : end;
    }
    if (len == 0) { // "." itself
        out[len++] = '.';
        out[len++] = '/';
    }
    out[len] = '\0';
    return out;
}

static bool same_path(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a == '\\' ? '/' : *a;
        char y = *b == '\\' ? '/' : *b;
#ifdef _WIN32
        x = (char)tolower((unsigned char)x); // Windows paths ignore case
        y = (char)tolower((unsigned char)y);
#endif
        if (x != y) return false;
        if (x == '\0') return true;
    }
}

static bool folder_exists(const char *folder)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesA(folder);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    return stat(folder, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

char *packages_relative(const char *from, const char *path)
{
    char *a = normalize_folder(from);
    char *b = normalize_folder(path);
    // The parts they share, whole ones only.
    size_t common = 0;
    for (size_t i = 0; a[i] && b[i]; i++) {
        char x = a[i];
        char y = b[i];
#ifdef _WIN32
        x = (char)tolower((unsigned char)x);
        y = (char)tolower((unsigned char)y);
#endif
        if (x != y) break;
        if (x == '/') common = i + 1;
    }
    if (common == 0) { // Another drive, or nothing in common
        free(a);
        size_t n = strlen(b);
        if (n > 1) b[n - 1] = '\0'; // Without its last '/'
        return b;
    }
    size_t ups = 0;
    for (const char *p = a + common; *p; p++) ups += *p == '/';
    const char *rest = b + common;
    char *out = malloc(ups * 3 + strlen(rest) + 3);
    if (!out) abort();
    size_t len = 0;
    if (ups == 0) {
        out[len++] = '.';
        out[len++] = '/';
    }
    for (size_t i = 0; i < ups; i++) {
        memcpy(out + len, "../", 3);
        len += 3;
    }
    memcpy(out + len, rest, strlen(rest) + 1);
    len += strlen(rest);
    if (len > 2 && out[len - 1] == '/') out[--len] = '\0'; // "../shared", not "../shared/"
    if (len == 2) out[1] = '\0';                              // ".", not "./"
    free(a);
    free(b);
    return out;
}

// ---------------------------------------------------------------------------
// Sources

bool packages_is_local(const char *source)
{
    return source[0] == '.' || is_absolute(source);
}

// A part of a git source: a host, owner, repository or folder name.
static bool valid_part(const char *p, const size_t n)
{
    if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = p[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

// Whether `text` (`n` long) is parts with one '/' between them, at least
// `min` of them.
static bool valid_parts(const char *text, const size_t n, const int min)
{
    int parts = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && text[i] != '/') continue;
        if (!valid_part(text + start, i - start)) return false;
        parts++;
        start = i + 1;
    }
    return parts >= min;
}

// github.com/owner/repo, maybe with //sub/folder: a host (with a '.'), then
// at least an owner and a repository.
static bool valid_git_source(const char *source)
{
    const char *sub = strstr(source, "//");
    const size_t repo = sub ? (size_t)(sub - source) : strlen(source);
    const char *slash = memchr(source, '/', repo);
    if (!slash || !memchr(source, '.', (size_t)(slash - source))) return false;
    if (!valid_parts(source, repo, 3)) return false;
    return !sub || valid_parts(sub + 2, strlen(sub + 2), 1);
}

char *packages_repo(const char *source)
{
    const char *sub = strstr(source, "//");
    return sub ? dup_n(source, (size_t)(sub - source)) : dup(source);
}

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool packages_parse_source(const char *text, char **source, char **ref)
{
    *source = NULL;
    *ref = NULL;
    while (isspace((unsigned char)*text)) text++;
    size_t n = strlen(text);
    while (n > 0 && isspace((unsigned char)text[n - 1])) n--;
    if (n == 0) return false;
    char *s = dup_n(text, n);
    if (packages_is_local(s)) {
        for (char *c = s; *c; c++) {
            if (*c == '\\') *c = '/';
        }
        *source = s;
        return true;
    }

    // https://, ssh://git@ and git@host:owner/repo (scp's way of writing it)
    const char *p = s;
    static const char *const schemes[] = {"https://", "http://", "git://", "ssh://"};
    for (size_t i = 0; i < sizeof schemes / sizeof schemes[0]; i++) {
        if (starts_with(p, schemes[i])) p += strlen(schemes[i]);
    }
    const bool scp = starts_with(p, "git@");
    if (scp) p += 4;
    char *rest = dup(p);
    free(s);
    if (scp) {
        char *colon = strchr(rest, ':');
        if (colon && (!strchr(rest, '/') || colon < strchr(rest, '/'))) *colon = '/';
    }
    // @ref, after the host
    char *slash = strchr(rest, '/');
    char *at = slash ? strrchr(slash, '@') : NULL;
    if (at) {
        *at = '\0';
        if (!at[1]) {
            free(rest);
            return false;
        }
        *ref = dup(at + 1);
    }
    // The repository without .git or a '/' at the end, and its folder
    char *sub = strstr(rest, "//");
    char *sub_text = NULL;
    if (sub) {
        sub_text = dup(sub + 2);
        *sub = '\0';
        size_t m = strlen(sub_text);
        while (m > 0 && sub_text[m - 1] == '/') sub_text[--m] = '\0';
        if (m == 0) { // "//" and no folder after it
            free(sub_text);
            free(rest);
            free(*ref);
            *ref = NULL;
            return false;
        }
    }
    size_t m = strlen(rest);
    while (m > 0 && rest[m - 1] == '/') rest[--m] = '\0';
    if (m > 4 && strcmp(rest + m - 4, ".git") == 0) rest[m - 4] = '\0';
    for (char *c = rest; *c && *c != '/'; c++) *c = (char)tolower((unsigned char)*c); // Hosts ignore case
    char *joined = sub_text && sub_text[0] ? format("%s//%s", rest, sub_text) : dup(rest);
    free(rest);
    free(sub_text);
    if (!valid_git_source(joined)) {
        free(joined);
        free(*ref);
        *ref = NULL;
        return false;
    }
    *source = joined;
    return true;
}

static bool valid_commit(const char *text)
{
    const size_t n = strlen(text);
    if (n != 40 && n != 64) return false; // SHA-1, or SHA-256 repositories'
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)text[i])) return false;
    }
    return true;
}

// Package names are namespaces: Physics, or Tide.Physics.
static bool valid_name(const char *name)
{
    bool start = true;
    for (const char *c = name; *c; c++) {
        if (start && !(isalpha((unsigned char)*c) || *c == '_')) return false;
        if (*c == '.') {
            start = true;
            continue;
        }
        if (!isalnum((unsigned char)*c) && *c != '_') return false;
        start = false;
    }
    return !start;
}

// ---------------------------------------------------------------------------
// tide.packages

static void report_at(const packages_report_fn report, void *user, const char *where, const int line,
                      const char *note, const char *fmt, ...)
{
    if (!report) return;
    va_list args;
    va_start(args, fmt);
    char message[1024];
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    report(user, where, line, message, note);
}

void packages_free_file(packages_file *f)
{
    for (int i = 0; i < f->count; i++) {
        free(f->lines[i].source);
        free(f->lines[i].ref);
        free(f->lines[i].commit);
    }
    free(f->lines);
    free(f->path);
    free(f->folder);
    free(f->name);
    memset(f, 0, sizeof *f);
}

// Its words, up to `max`: false if there are more.
static int split_words(char *line, char **words, const int max)
{
    int n = 0;
    for (char *p = line; *p;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (n == max) return max + 1;
        words[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

bool packages_read(const char *folder, packages_file *out, const packages_report_fn report, void *user)
{
    memset(out, 0, sizeof *out);
    out->folder = normalize_folder(folder);
    out->path = format("%s%s", out->folder, PACKAGES_FILE);
    char *text = read_text(out->path);
    if (!text) return true;
    out->exists = true;
    bool ok = true;
    const char *path = out->path;
    int number = 0;
    for (char *line = text; *line;) {
        number++;
        char *end = line + strcspn(line, "\n");
        char *next = *end ? end + 1 : end;
        *end = '\0';
        // A comment starts the line, or follows a space
        for (char *c = line; *c; c++) {
            if (*c == '#' && (c == line || isspace((unsigned char)c[-1]))) {
                *c = '\0';
                break;
            }
        }
        char *words[3];
        const int count = split_words(line, words, 2);
        line = next;
        if (count == 0) continue;
        if (count > 2) {
            report_at(report, user, path, number, "a line is a package and its commit, or `package Name`, or `tide 0.3`",
                      "this line has more than two words");
            ok = false;
            continue;
        }
        if (strcmp(words[0], "package") == 0) {
            if (count != 2 || !valid_name(words[1])) {
                report_at(report, user, path, number, "a package's name is its namespace, like `package Physics`",
                          "`package` needs a name");
                ok = false;
            } else if (out->name) {
                report_at(report, user, path, number, NULL, "this package is already called %s", out->name);
                ok = false;
            } else {
                out->name = dup(words[1]);
            }
            continue;
        }
        if (strcmp(words[0], "tide") == 0) {
            int version[3];
            if (count != 2 || !tide_version_parse(words[1], (int)strlen(words[1]), version)) {
                report_at(report, user, path, number, "write the oldest version it builds with, like `tide 0.3`",
                          "`tide` needs a version");
                ok = false;
            } else if (out->tide_line) {
                report_at(report, user, path, number, NULL, "the version of tide is already given on line %d",
                          out->tide_line);
                ok = false;
            } else {
                memcpy(out->tide, version, sizeof version);
                out->tide_line = number;
            }
            continue;
        }

        package_line l = {number, NULL, NULL, NULL, packages_is_local(words[0])};
        if (l.local) {
            if (count == 2) {
                report_at(report, user, path, number, "a folder's files are used as they are, so there's no commit",
                          "a folder on this machine needs no commit");
                ok = false;
                continue;
            }
            l.source = dup(words[0]);
            for (char *c = l.source; *c; c++) {
                if (*c == '\\') *c = '/';
            }
        } else {
            char *source = NULL;
            char *ref = NULL;
            if (!packages_parse_source(words[0], &source, &ref)) {
                report_at(report, user, path, number,
                          "a package is a git repository and its commit, like `github.com/owner/repo 0123abc...`, "
                          "or a folder, like `../shared`",
                          "'%s' isn't a package", words[0]);
                ok = false;
                continue;
            }
            l.source = source;
            l.ref = ref;
            if (count == 2) {
                if (!valid_commit(words[1])) {
                    report_at(report, user, path, number, "a commit is written in full: its 40 hex digits",
                              "'%s' isn't a commit", words[1]);
                    ok = false;
                } else {
                    l.commit = dup(words[1]);
                    for (char *c = l.commit; *c; c++) *c = (char)tolower((unsigned char)*c);
                }
            }
        }
        bool repeated = false;
        for (int i = 0; i < out->count && !repeated; i++) {
            if (l.local && out->lines[i].local) { // ../shared and ../shared/ are one folder
                char *a = normalize_folder(out->lines[i].source);
                char *b = normalize_folder(l.source);
                repeated = same_path(a, b);
                free(a);
                free(b);
            } else {
                repeated = !l.local && !out->lines[i].local && strcmp(out->lines[i].source, l.source) == 0;
            }
            if (repeated) {
                char note[64];
                snprintf(note, sizeof note, "it's on line %d already; keep one", out->lines[i].line);
                report_at(report, user, path, number, note, "%s is listed twice", l.source);
            }
        }
        if (repeated) {
            free(l.source);
            free(l.ref);
            free(l.commit);
            ok = false;
            continue;
        }
        out->lines = realloc(out->lines, sizeof(package_line) * (size_t)(out->count + 1));
        if (!out->lines) abort();
        out->lines[out->count++] = l;
    }
    free(text);
    return ok;
}

// ---------------------------------------------------------------------------
// Where packages are

char *packages_cache(void)
{
    const char *custom = getenv("TIDE_PACKAGES");
    char *dir;
    if (custom && custom[0]) {
        dir = dup(custom);
    } else {
#ifdef _WIN32
        const char *base = getenv("LOCALAPPDATA");
        dir = format("%s/Tide/packages", base && base[0] ? base : ".");
#else
        const char *home = getenv("HOME");
        dir = format("%s/.tide/packages", home && home[0] ? home : ".");
#endif
    }
    char *out = normalize_folder(dir);
    free(dir);
    return out;
}

char *packages_repo_dir(const package_line *line)
{
    if (line->local || !line->commit) return NULL;
    char *cache = packages_cache();
    char *repo = packages_repo(line->source);
    char *dir = format("%s%s/%s/", cache, repo, line->commit);
    free(cache);
    free(repo);
    return dir;
}

char *packages_folder(const package_line *line, const char *base)
{
    if (line->local) {
        if (is_absolute(line->source)) return normalize_folder(line->source);
        char *joined = format("%s%s", base, line->source);
        char *out = normalize_folder(joined);
        free(joined);
        return out;
    }
    char *dir = packages_repo_dir(line);
    const char *sub = strstr(line->source, "//");
    if (!dir || !sub) return dir;
    char *joined = format("%s%s", dir, sub + 2);
    free(dir);
    char *out = normalize_folder(joined);
    free(joined);
    return out;
}

// The same package, as two lines name it: the same folder, or the same
// source. `a_base` and `b_base` are where each line's folder is from.
static bool same_package(const package_line *a, const char *a_base, const package_line *b, const char *b_base)
{
    if (a->local != b->local) return false;
    if (!a->local) return strcmp(a->source, b->source) == 0;
    char *x = packages_folder(a, a_base);
    char *y = packages_folder(b, b_base);
    const bool same = same_path(x, y);
    free(x);
    free(y);
    return same;
}

// The text of a line for `line` in the file in `folder`: how tide.packages
// writes it.
static char *line_text(const package_line *line, const char *line_base, const char *folder)
{
    if (line->local) {
        char *target = packages_folder(line, line_base);
        char *out = packages_relative(folder, target);
        free(target);
        return out;
    }
    return format("%s%s%s%s%s", line->source, line->ref ? "@" : "", line->ref ? line->ref : "", line->commit ? " " : "",
                  line->commit ? line->commit : "");
}

void packages_free(game_packages *g)
{
    for (int i = 0; i < g->count; i++) {
        free(g->items[i].name);
        free(g->items[i].folder);
    }
    free(g->items);
    packages_free_file(&g->file);
    memset(g, 0, sizeof *g);
}

typedef struct need_list {
    package_need *items;
    int count;
} need_list;

static bool resolve(const char *folder, game_packages *out, const packages_fetch_fn fetch,
                    const packages_report_fn report, void *user, need_list *missing);

bool packages_resolve(const char *folder, game_packages *out, const packages_fetch_fn fetch,
                      const packages_report_fn report, void *user)
{
    return resolve(folder, out, fetch, report, user, NULL);
}

int packages_missing(const char *folder, const packages_fetch_fn fetch, void *user, package_need **out)
{
    need_list missing = {0};
    game_packages g;
    resolve(folder, &g, fetch, NULL, user, &missing);
    packages_free(&g);
    *out = missing.items;
    return missing.count;
}

void packages_free_needs(package_need *needs, const int count)
{
    for (int i = 0; i < count; i++) {
        free(needs[i].line);
        free(needs[i].by);
    }
    free(needs);
}

// packages_resolve, which also lists what packages need that the game doesn't
// list in `missing`, if it isn't NULL.
static bool resolve(const char *folder, game_packages *out, const packages_fetch_fn fetch,
                    const packages_report_fn report, void *user, need_list *missing)
{
    memset(out, 0, sizeof *out);
    bool ok = packages_read(folder, &out->file, report, user);
    const packages_file *game = &out->file;
    if (game->tide_line && !tide_version_at_least(game->tide[0], game->tide[1], game->tide[2])) {
        report_at(report, user, game->path, game->tide_line, "run `tide upgrade`",
                  "this needs tide %d.%d or newer, and this is %s", game->tide[0], game->tide[1], tide_version_text());
        ok = false;
    }
    if (game->count == 0) return ok;

    // Each package the game lists, with its own tide.packages.
    packages_file *own = calloc((size_t)game->count, sizeof(packages_file));
    int *of = calloc((size_t)game->count, sizeof(int)); // The game's line of each package found
    if (!own || !of) abort();
    out->items = calloc((size_t)game->count, sizeof(package));
    if (!out->items) abort();
    int found = 0;
    for (int i = 0; i < game->count; i++) {
        const package_line *line = &game->lines[i];
        if (!line->local && !line->commit) {
            char note[600];
            snprintf(note, sizeof note, "`tide update %s` pins it to the newest commit%s%s", line->source,
                     line->ref ? " of " : "", line->ref ? line->ref : "");
            report_at(report, user, game->path, line->line, note, "%s has no commit to build with", line->source);
            ok = false;
            continue;
        }
        char *dir = packages_folder(line, game->folder);
        if (!line->local && !folder_exists(dir)) {
            char *repo = packages_repo_dir(line);
            bool here = folder_exists(repo);
            if (!here && fetch) here = fetch(user, line, repo);
            free(repo);
            if (!here) { // fetch said why; tidels, with nothing to get it with, leaves it out
                ok = false;
                free(dir);
                continue;
            }
        }
        if (!folder_exists(dir)) {
            report_at(report, user, game->path, line->line, NULL, "%s isn't a folder%s", line->source,
                      line->local ? "" : " of that commit");
            ok = false;
            free(dir);
            continue;
        }
        packages_file *pf = &own[found];
        if (!packages_read(dir, pf, report, user)) ok = false;
        if (!pf->name) {
            report_at(report, user, game->path, line->line,
                      "a package's tide.packages starts with its name, which is its namespace: `package Physics`",
                      pf->exists ? "%s isn't a package: its tide.packages has no `package` line"
                                 : "%s isn't a package: it has no tide.packages",
                      line->source);
            ok = false;
            packages_free_file(pf);
            free(dir);
            continue;
        }
        if (pf->tide_line && !tide_version_at_least(pf->tide[0], pf->tide[1], pf->tide[2])) {
            char note[600] = "run `tide upgrade`";
            if (!line->local) {
                snprintf(note, sizeof note, "run `tide upgrade`, or use a commit of %s made for tide %s", line->source,
                         tide_version_text());
            }
            report_at(report, user, game->path, line->line, note, "%s needs tide %d.%d or newer, and this is %s",
                      pf->name, pf->tide[0], pf->tide[1], tide_version_text());
            ok = false;
        }
        bool taken = false;
        for (int k = 0; k < out->count && !taken; k++) {
            taken = strcmp(out->items[k].name, pf->name) == 0;
            if (taken) {
                report_at(report, user, game->path, line->line, "a game can't have two packages of one name",
                          "%s and %s are both called %s", out->items[k].line->source, line->source, pf->name);
            }
        }
        if (taken) {
            ok = false;
            packages_free_file(pf);
            free(dir);
            continue;
        }
        out->items[out->count++] = (package){dup(pf->name), dir, line};
        of[found++] = i;
    }

    // What each needs, which the game must list: needs[k * found + j] when
    // package k needs package j.
    bool *needs = calloc((size_t)found * (size_t)found + 1, sizeof(bool));
    if (!needs) abort();
    for (int k = 0; k < found; k++) {
        const packages_file *pf = &own[k];
        for (int r = 0; r < pf->count; r++) {
            const package_line *need = &pf->lines[r];
            if (need->local && !out->items[k].line->local) {
                report_at(report, user, pf->path, need->line,
                          "name it by its git repository and commit, like the game does",
                          "a package from git can't use a folder on this machine (%s)", need->source);
                ok = false;
                continue;
            }
            int j = -1;
            for (int g = 0; g < game->count && j < 0; g++) {
                if (!same_package(need, pf->folder, &game->lines[g], game->folder)) continue;
                for (int m = 0; m < found; m++) {
                    if (of[m] == g) j = m;
                }
                if (j < 0) j = found; // Listed, but not found: reported already
            }
            if (j < 0) {
                char *text = line_text(need, pf->folder, game->folder);
                char note[1200];
                snprintf(note, sizeof note, "add this line: %s", text);
                report_at(report, user, game->path, out->items[k].line->line, note,
                          "%s needs %s, which this file doesn't list", out->items[k].name,
                          need->local ? text : need->source); // A folder as the game's file writes it
                bool listed = false;
                for (int m = 0; missing && m < missing->count && !listed; m++) {
                    listed = strcmp(missing->items[m].line, text) == 0;
                }
                if (missing && !listed) {
                    const package_need n = {text, dup(out->items[k].name)};
                    missing->items = realloc(missing->items, sizeof(package_need) * (size_t)(missing->count + 1));
                    if (!missing->items) abort();
                    missing->items[missing->count++] = n;
                } else {
                    free(text);
                }
                ok = false;
            } else if (j < found && j != k) {
                needs[k * found + j] = true;
            }
        }
    }

    // Each after the packages it needs; of those that could go next, the
    // first by name; and in a circle of needs, the first of it by name.
    package *ordered = calloc((size_t)found + 1, sizeof(package));
    bool *placed = calloc((size_t)found + 1, sizeof(bool));
    if (!ordered || !placed) abort();
    for (int n = 0; n < found; n++) {
        int pick = -1;
        for (int pass = 0; pass < 2 && pick < 0; pass++) {
            for (int k = 0; k < found; k++) {
                if (placed[k]) continue;
                bool ready = true;
                for (int j = 0; j < found && ready && pass == 0; j++) ready = placed[j] || !needs[k * found + j];
                if (ready && (pick < 0 || strcmp(out->items[k].name, out->items[pick].name) < 0)) pick = k;
            }
        }
        placed[pick] = true;
        ordered[n] = out->items[pick];
    }
    memcpy(out->items, ordered, sizeof(package) * (size_t)found);
    free(ordered);
    free(placed);
    free(needs);
    for (int k = 0; k < found; k++) packages_free_file(&own[k]);
    free(own);
    free(of);
    return ok;
}
