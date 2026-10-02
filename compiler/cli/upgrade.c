#include "upgrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "release.h"
#include "sha256.h"
#include "sys.h"

#ifndef TIDE_VERSION
#define TIDE_VERSION "0.0.0-dev"
#endif
#ifndef TIDE_CHANNEL
#define TIDE_CHANNEL "nightly"
#endif
#ifndef TIDE_REPOSITORY
#define TIDE_REPOSITORY "BlenMiner/tide-engine"
#endif

// The package for this machine, as the release workflow names it.
#if defined(_WIN32)
#define PACKAGE "tide-windows-x64.zip"
#elif defined(__APPLE__)
#define PACKAGE "tide-macos-arm64.tar.gz"
#else
#define PACKAGE "tide-linux-x64.tar.gz"
#endif

static bool is_dev_build(void)
{
    const char *version = TIDE_VERSION;
    const size_t n = strlen(version);
    return n >= 4 && strcmp(version + n - 4, "-dev") == 0;
}

const char *tide_channel(const char *root)
{
    static char channel[32];
    char *path = path_join(root, "channel");
    char *text = sys_read_file(path, NULL);
    free(path);
    snprintf(channel, sizeof channel, "%s", text ? text : TIDE_CHANNEL);
    free(text);
    for (char *c = channel; *c; c++) {
        if (*c == '\r' || *c == '\n' || *c == ' ') *c = '\0';
    }
    return strcmp(channel, "stable") == 0 || strcmp(channel, "nightly") == 0 ? channel : TIDE_CHANNEL;
}

// Downloads `url` to `path`.
static bool download(const char *url, const char *path, const bool quiet)
{
    const char *curl = sys_tool("curl");
    const char *const argv[] = {curl, "-fsSL", "--retry", "2", "-m", quiet ? "5" : "600",
                                "-H", "Accept: application/vnd.github+json", "-o", path, url, NULL};
    return sys_run(argv, NULL, quiet) == 0;
}

#define RELEASES_API "https://api.github.com/repos/" TIDE_REPOSITORY "/releases"

// JSON from `url`, saved in `work` as `name`; NULL if it didn't come.
static const json *fetch_json(const char *work, const char *url, const char *name)
{
    char *path = path_join(work, name);
    const bool ok = download(url, path, true);
    size_t len = 0;
    char *text = ok ? sys_read_file(path, &len) : NULL;
    free(path);
    return text ? json_parse(text, len) : NULL;
}

// The GitHub releases list, newest first: the last 100 published, which
// always include the newest nightly.
static const json *fetch_releases(const char *work, const bool quiet)
{
    char *list = path_join(work, "releases.json");
    const char *custom = sys_env("TIDE_RELEASES_URL"); // For tests: a file:// URL to a releases list
    const char *url = custom ? custom : RELEASES_API "?per_page=100";
    const bool ok = download(url, list, quiet);
    if (!ok) {
        if (!quiet) fprintf(stderr, "tide: couldn't reach GitHub to look for new versions\n");
        free(list);
        return NULL;
    }
    size_t len = 0;
    char *text = sys_read_file(list, &len);
    free(list);
    const json *releases = text ? json_parse(text, len) : NULL;
    if (!releases || releases->kind != JSON_ARRAY) {
        if (!quiet) fprintf(stderr, "tide: GitHub's list of releases didn't make sense\n");
        return NULL;
    }
    return releases;
}

// The newest version of `channel` (see tide_release_pick). Stable asks GitHub
// for its latest release too: nightly ones can push it out of the list.
static const json *newest(const char *work, const json *releases, const char *channel)
{
    const bool ask = strcmp(channel, "stable") == 0 && !sys_env("TIDE_RELEASES_URL");
    const json *latest = ask ? fetch_json(work, RELEASES_API "/latest", "latest.json") : NULL;
    return tide_release_pick(releases, latest, channel);
}

// The release of `version`, in the list or, for an older one, asked by its tag.
static const json *exactly(const char *work, const json *releases, const char *version)
{
    const json *found = tide_release_find(releases, version);
    if (found || sys_env("TIDE_RELEASES_URL")) return found;
    char url[512];
    snprintf(url, sizeof url, RELEASES_API "/tags/v%s", version);
    const json *release = fetch_json(work, url, "tag.json");
    return tide_release_is(release, version) ? release : NULL;
}

static const json *find_asset(const json *release, const char *name)
{
    const json *assets = json_get(release, "assets");
    for (int i = 0; assets && i < assets->count; i++) {
        const char *asset = json_str(json_get(assets->items[i], "name"));
        if (asset && strcmp(asset, name) == 0) return assets->items[i];
    }
    return NULL;
}

static const char *asset_url(const json *release, const char *name)
{
    return json_str(json_get(find_asset(release, name), "browser_download_url"));
}

// The asset's size in bytes, as GitHub gives it; 0 if it doesn't.
static int64_t asset_size(const json *release, const char *name)
{
    const json *size = json_get(find_asset(release, name), "size");
    return size && size->kind == JSON_NUMBER && size->number > 0 ? (int64_t)size->number : 0;
}

// Draws the download's progress over the last one drawn: a bar when the
// package's size is known, a spinner when it isn't.
static void draw_progress(int64_t got, const int64_t size, const int64_t ms, const int frame, const bool done)
{
    const double mb = 1024.0 * 1024.0;
    const double seconds = (double)ms / 1000.0;
    if (got < 0) got = 0;
    if (size > 0 && got > size) got = size;
    char line[128];
    int n;
    if (size > 0) {
        enum { WIDTH = 30 };
        const int filled = (int)(got * WIDTH / size);
        char bar[WIDTH + 1];
        for (int i = 0; i < WIDTH; i++) bar[i] = i < filled ? '=' : i == filled ? '>' : ' ';
        bar[WIDTH] = '\0';
        n = snprintf(line, sizeof line, "  [%s] %3d%%  %.1f / %.1f MB", bar, (int)(got * 100 / size), got / mb, size / mb);
    } else if (done) {
        n = snprintf(line, sizeof line, "  %.1f MB", got / mb);
    } else {
        n = snprintf(line, sizeof line, "  %c %.1f MB", "|/-\\"[frame % 4], got / mb);
    }
    if (done) snprintf(line + n, sizeof line - (size_t)n, "  in %.1fs", seconds);
    else if (seconds >= 1) snprintf(line + n, sizeof line - (size_t)n, "  %.1f MB/s", got / mb / seconds);
    printf("\r%-72s", line);
    fflush(stdout);
}

// Downloads the package to `path`, with a progress bar in a terminal. curl
// runs in the background, and the bar follows the file as it grows; curl's
// errors wait in a log, to be shown once the bar is done with the line.
static bool download_package(const char *url, const char *path, const char *work, const int64_t size)
{
    if (!sys_is_terminal()) return download(url, path, false);
    char *log = path_join(work, "curl.log");
    const char *const argv[] = {sys_tool("curl"), "-fsSL", "--retry", "2", "-m", "600",
                                "--stderr", log, "-o", path, url, NULL};
    sys_process *curl = sys_start(argv, NULL);
    if (!curl) {
        free(log);
        return false;
    }
    const int64_t start = sys_now_ms();
    int code = 0;
    for (int frame = 0;; frame++) {
        const bool ended = sys_wait(curl, 100, &code);
        if (!ended || code == 0) draw_progress(sys_file_size(path), size, sys_now_ms() - start, frame, ended);
        if (ended) break;
    }
    printf("\n");
    fflush(stdout);
    if (code != 0) {
        char *errors = sys_read_file(log, NULL);
        if (errors) fputs(errors, stderr);
        free(errors);
    }
    free(log);
    return code == 0;
}

// The hash SHA256SUMS lists for `file` ("<hash>  <file>" lines).
static bool expected_hash(const char *sums, const char *file, char out[65])
{
    for (const char *line = sums; line && *line;) {
        const char *end = strchr(line, '\n');
        const size_t n = end ? (size_t)(end - line) : strlen(line);
        const char *name = memchr(line, ' ', n);
        if (name && name - line == 64) {
            while (*name == ' ' || *name == '*') name++;
            const size_t name_len = n - (size_t)(name - line);
            const size_t file_len = strlen(file);
            if (name_len >= file_len && strncmp(name, file, file_len) == 0) {
                memcpy(out, line, 64);
                out[64] = '\0';
                return true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Swapping the files

typedef struct swap {
    const char *root;
    const char *from;
    bool ok;
} swap;

// tide and tidels may be running (this very tide, or an editor's tidels).
// Windows can't overwrite them, but it can rename them, so the old ones move
// aside and tide_cleanup deletes them later.
static void swap_program(void *user, const char *name, const bool is_dir)
{
    swap *s = user;
    if (is_dir) return;
    char *from = path_join(s->from, name);
    char *bin = path_join(s->root, "bin");
    char *to = path_join(bin, name);
    if (sys_exists(to)) {
        char aside[1024];
        snprintf(aside, sizeof aside, "%s.old-%lld", to, (long long)sys_now());
        if (!sys_rename(to, aside)) s->ok = false;
    }
    if (s->ok && !sys_rename(from, to)) s->ok = false;
    free(from);
    free(bin);
    free(to);
}

static void swap_entry(void *user, const char *name, const bool is_dir)
{
    swap *s = user;
    char *from = path_join(s->from, name);
    if (is_dir && strcmp(name, "bin") == 0) {
        char *bin = path_join(s->root, "bin");
        sys_mkdirs(bin);
        swap bin_swap = {s->root, from, true};
        sys_list(from, swap_program, &bin_swap);
        s->ok &= bin_swap.ok;
        free(bin);
    } else {
        char *to = path_join(s->root, name);
        char old[1024];
        snprintf(old, sizeof old, "%s.old", to);
        sys_remove_tree(old);
        if (sys_exists(to) && !sys_rename(to, old)) s->ok = false;
        if (s->ok && !sys_rename(from, to)) s->ok = false;
        sys_remove_tree(old);
        free(to);
    }
    free(from);
}

static void remove_old(void *user, const char *name, const bool is_dir)
{
    const char *bin = user;
    if (is_dir || !strstr(name, ".old-")) return;
    char *path = path_join(bin, name);
    sys_remove(path); // Fails while that old copy still runs; the next run tries again
    free(path);
}

void tide_cleanup(const char *root)
{
    char *bin = path_join(root, "bin");
    sys_list(bin, remove_old, bin);
    free(bin);
}

// ---------------------------------------------------------------------------

int tide_upgrade(const char *root, const char *channel, const char *version)
{
    if (version && version[0] == 'v') version++;
    if (version && !tide_version_valid(version)) {
        fprintf(stderr, "tide: '%s' isn't a version\n", version);
        fprintf(stderr, "  = note: versions look like 0.2.0, or 0.2.0-nightly.3 for nightly ones\n");
        return 2;
    }
    // Switching channels, it installs the other one's newest even if it's older.
    const bool switching = channel && strcmp(channel, tide_channel(root)) != 0;
    if (!channel) channel = tide_channel(root);
    char *work = path_join(root, ".upgrade");
    sys_remove_tree(work);
    if (!sys_mkdirs(work)) {
        fprintf(stderr, "tide: can't write to %s, where tide is installed\n", root);
        return 1;
    }

    const json *releases = fetch_releases(work, false);
    if (!releases) return 1;
    const json *release = version ? exactly(work, releases, version) : newest(work, releases, channel);
    if (!release) {
        if (version) fprintf(stderr, "tide: there's no release called %s\n", version);
        else fprintf(stderr, "tide: there's no %s release yet\n", channel);
        return 1;
    }
    const char *next = tide_release_version(release);
    // Upgrading only ever goes forward; an exact version or another channel is what was asked for.
    const int order = tide_version_compare(next, TIDE_VERSION);
    if (order == 0 || (order < 0 && !version && !switching)) {
        if (order == 0 && version) printf("tide %s is already installed.\n", TIDE_VERSION);
        else if (order == 0) printf("tide %s is the newest %s version.\n", TIDE_VERSION, channel);
        else printf("tide %s is newer than any %s version out (%s).\n", TIDE_VERSION, channel, next);
        char *channel_file = path_join(root, "channel");
        sys_write_text(channel_file, channel);
        sys_remove_tree(work);
        return 0;
    }

    const char *package_url = asset_url(release, PACKAGE);
    const char *sums_url = asset_url(release, "SHA256SUMS");
    if (!package_url || !sums_url) {
        fprintf(stderr, "tide: release %s has no package for this platform (%s)\n", next, PACKAGE);
        return 1;
    }
    printf("Downloading tide %s...\n", next);
    fflush(stdout);
    char *package = path_join(work, PACKAGE);
    char *sums_path = path_join(work, "SHA256SUMS");
    if (!download_package(package_url, package, work, asset_size(release, PACKAGE)) ||
        !download(sums_url, sums_path, false)) {
        fprintf(stderr, "tide: the download failed\n");
        return 1;
    }

    // The package must match the checksum published with it.
    char *sums = sys_read_file(sums_path, NULL);
    char want[65];
    if (!sums || !expected_hash(sums, PACKAGE, want)) {
        fprintf(stderr, "tide: SHA256SUMS doesn't list %s\n", PACKAGE);
        return 1;
    }
    size_t len = 0;
    char *data = sys_read_file(package, &len);
    char got[65];
    sha256_hex(data, len, got);
    free(data);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "tide: the download is damaged (its checksum doesn't match); nothing was changed\n");
        return 1;
    }

    char *fresh = path_join(work, "new");
    sys_mkdirs(fresh);
    const char *const tar[] = {sys_tool("tar"), "-xf", package, "-C", fresh, NULL};
    if (sys_run(tar, NULL, false) != 0) {
        fprintf(stderr, "tide: couldn't unpack the download\n");
        return 1;
    }
    swap s = {root, fresh, true};
    sys_list(fresh, swap_entry, &s);
    if (!s.ok) {
        fprintf(stderr, "tide: couldn't replace every file in %s; run `tide upgrade` again\n", root);
        return 1;
    }
    char *channel_file = path_join(root, "channel");
    sys_write_text(channel_file, channel);
    sys_remove_tree(work);
    printf("Upgraded tide %s -> %s (%s).\n", TIDE_VERSION, next, channel);
    fflush(stdout);
    // The new tide updates the editor extension, in the editors that have it.
#ifdef _WIN32
    char *tide = path_join(root, "bin/tide.exe");
#else
    char *tide = path_join(root, "bin/tide");
#endif
    const char *const editors[] = {tide, "editors", "--update", NULL};
    sys_run(editors, NULL, false);
    free(tide);
    const char *notes = json_str(json_get(release, "html_url"));
    if (notes) printf("What's new: %s\n", notes);
    return 0;
}

void tide_check_for_update(const char *root)
{
    if (is_dev_build() || sys_env("TIDE_NO_UPDATE_CHECK")) return;
    char *stamp = path_join(root, ".last-update-check");
    char *text = sys_read_file(stamp, NULL);
    const long long last = text ? atoll(text) : 0;
    free(text);
    if (sys_now() - last < 24 * 60 * 60) {
        free(stamp);
        return;
    }
    char now[32];
    snprintf(now, sizeof now, "%lld", (long long)sys_now());
    sys_write_text(stamp, now);
    free(stamp);

    char *work = path_join(root, ".update-check");
    sys_mkdirs(work);
    const json *releases = fetch_releases(work, true);
    const char *channel = tide_channel(root);
    const json *release = releases ? newest(work, releases, channel) : NULL;
    if (release && tide_version_compare(tide_release_version(release), TIDE_VERSION) > 0) {
        fprintf(stderr, "\ntide %s is out (you have %s): run `tide upgrade`\n", tide_release_version(release), TIDE_VERSION);
    }
    sys_remove_tree(work);
}
