#include "upgrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "release.h"
#include "sha256.h"
#include "sys.h"

#ifndef PURR_VERSION
#define PURR_VERSION "0.0.0-dev"
#endif
#ifndef PURR_CHANNEL
#define PURR_CHANNEL "nightly"
#endif
#ifndef PURR_REPOSITORY
#define PURR_REPOSITORY "BlenMiner/PurrEngine"
#endif

// The package for this machine, as the release workflow names it.
#if defined(_WIN32)
#define PACKAGE "purr-windows-x64.zip"
#elif defined(__APPLE__)
#define PACKAGE "purr-macos-arm64.tar.gz"
#else
#define PACKAGE "purr-linux-x64.tar.gz"
#endif

static bool is_dev_build(void)
{
    const char *version = PURR_VERSION;
    const size_t n = strlen(version);
    return n >= 4 && strcmp(version + n - 4, "-dev") == 0;
}

const char *purr_channel(const char *root)
{
    static char channel[32];
    char *path = path_join(root, "channel");
    char *text = sys_read_file(path, NULL);
    free(path);
    snprintf(channel, sizeof channel, "%s", text ? text : PURR_CHANNEL);
    free(text);
    for (char *c = channel; *c; c++) {
        if (*c == '\r' || *c == '\n' || *c == ' ') *c = '\0';
    }
    return strcmp(channel, "stable") == 0 || strcmp(channel, "nightly") == 0 ? channel : PURR_CHANNEL;
}

// curl and tar ship with Windows 10+, macOS and Linux. On Windows, the ones
// in System32: Git's GNU tar, if it comes first on PATH, can't unpack zips.
static const char *system_tool(const char *name)
{
#ifdef _WIN32
    const char *windows = sys_env("SystemRoot");
    if (windows) {
        char file[64];
        snprintf(file, sizeof file, "System32/%s.exe", name);
        char *path = path_join(windows, file);
        if (sys_exists(path)) return path;
        free(path);
    }
#endif
    return name; // Found on PATH
}

// Downloads `url` to `path`.
static bool download(const char *url, const char *path, const bool quiet)
{
    const char *curl = system_tool("curl");
    const char *const argv[] = {curl, "-fsSL", "--retry", "2", "-m", quiet ? "5" : "600",
                                "-H", "Accept: application/vnd.github+json", "-o", path, url, NULL};
    return sys_run(argv, NULL, quiet) == 0;
}

#define RELEASES_API "https://api.github.com/repos/" PURR_REPOSITORY "/releases"

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
    const char *custom = sys_env("PURR_RELEASES_URL"); // For tests: a file:// URL to a releases list
    const char *url = custom ? custom : RELEASES_API "?per_page=100";
    const bool ok = download(url, list, quiet);
    if (!ok) {
        if (!quiet) fprintf(stderr, "purr: couldn't reach GitHub to look for new versions\n");
        free(list);
        return NULL;
    }
    size_t len = 0;
    char *text = sys_read_file(list, &len);
    free(list);
    const json *releases = text ? json_parse(text, len) : NULL;
    if (!releases || releases->kind != JSON_ARRAY) {
        if (!quiet) fprintf(stderr, "purr: GitHub's list of releases didn't make sense\n");
        return NULL;
    }
    return releases;
}

// The newest version of `channel` (see purr_release_pick). Stable asks GitHub
// for its latest release too: nightly ones can push it out of the list.
static const json *newest(const char *work, const json *releases, const char *channel)
{
    const bool ask = strcmp(channel, "stable") == 0 && !sys_env("PURR_RELEASES_URL");
    const json *latest = ask ? fetch_json(work, RELEASES_API "/latest", "latest.json") : NULL;
    return purr_release_pick(releases, latest, channel);
}

// The release of `version`, in the list or, for an older one, asked by its tag.
static const json *exactly(const char *work, const json *releases, const char *version)
{
    const json *found = purr_release_find(releases, version);
    if (found || sys_env("PURR_RELEASES_URL")) return found;
    char url[512];
    snprintf(url, sizeof url, RELEASES_API "/tags/v%s", version);
    const json *release = fetch_json(work, url, "tag.json");
    return purr_release_is(release, version) ? release : NULL;
}

static const char *asset_url(const json *release, const char *name)
{
    const json *assets = json_get(release, "assets");
    for (int i = 0; assets && i < assets->count; i++) {
        const char *asset = json_str(json_get(assets->items[i], "name"));
        if (asset && strcmp(asset, name) == 0) return json_str(json_get(assets->items[i], "browser_download_url"));
    }
    return NULL;
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

// purr and purrls may be running (this very purr, or an editor's purrls).
// Windows can't overwrite them, but it can rename them, so the old ones move
// aside and purr_cleanup deletes them later.
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

void purr_cleanup(const char *root)
{
    char *bin = path_join(root, "bin");
    sys_list(bin, remove_old, bin);
    free(bin);
}

// ---------------------------------------------------------------------------

int purr_upgrade(const char *root, const char *channel, const char *version)
{
    if (version && version[0] == 'v') version++;
    if (version && !purr_version_valid(version)) {
        fprintf(stderr, "purr: '%s' isn't a version\n", version);
        fprintf(stderr, "  = note: versions look like 0.2.0, or 0.2.0-nightly.3 for nightly ones\n");
        return 2;
    }
    // Switching channels, it installs the other one's newest even if it's older.
    const bool switching = channel && strcmp(channel, purr_channel(root)) != 0;
    if (!channel) channel = purr_channel(root);
    char *work = path_join(root, ".upgrade");
    sys_remove_tree(work);
    if (!sys_mkdirs(work)) {
        fprintf(stderr, "purr: can't write to %s, where purr is installed\n", root);
        return 1;
    }

    const json *releases = fetch_releases(work, false);
    if (!releases) return 1;
    const json *release = version ? exactly(work, releases, version) : newest(work, releases, channel);
    if (!release) {
        if (version) fprintf(stderr, "purr: there's no release called %s\n", version);
        else fprintf(stderr, "purr: there's no %s release yet\n", channel);
        return 1;
    }
    const char *next = purr_release_version(release);
    // Upgrading only ever goes forward; an exact version or another channel is what was asked for.
    const int order = purr_version_compare(next, PURR_VERSION);
    if (order == 0 || (order < 0 && !version && !switching)) {
        if (order == 0 && version) printf("purr %s is already installed.\n", PURR_VERSION);
        else if (order == 0) printf("purr %s is the newest %s version.\n", PURR_VERSION, channel);
        else printf("purr %s is newer than any %s version out (%s).\n", PURR_VERSION, channel, next);
        char *channel_file = path_join(root, "channel");
        sys_write_text(channel_file, channel);
        sys_remove_tree(work);
        return 0;
    }

    const char *package_url = asset_url(release, PACKAGE);
    const char *sums_url = asset_url(release, "SHA256SUMS");
    if (!package_url || !sums_url) {
        fprintf(stderr, "purr: release %s has no package for this platform (%s)\n", next, PACKAGE);
        return 1;
    }
    printf("Downloading purr %s...\n", next);
    fflush(stdout);
    char *package = path_join(work, PACKAGE);
    char *sums_path = path_join(work, "SHA256SUMS");
    if (!download(package_url, package, false) || !download(sums_url, sums_path, false)) {
        fprintf(stderr, "purr: the download failed\n");
        return 1;
    }

    // The package must match the checksum published with it.
    char *sums = sys_read_file(sums_path, NULL);
    char want[65];
    if (!sums || !expected_hash(sums, PACKAGE, want)) {
        fprintf(stderr, "purr: SHA256SUMS doesn't list %s\n", PACKAGE);
        return 1;
    }
    size_t len = 0;
    char *data = sys_read_file(package, &len);
    char got[65];
    sha256_hex(data, len, got);
    free(data);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "purr: the download is damaged (its checksum doesn't match); nothing was changed\n");
        return 1;
    }

    char *fresh = path_join(work, "new");
    sys_mkdirs(fresh);
    const char *const tar[] = {system_tool("tar"), "-xf", package, "-C", fresh, NULL};
    if (sys_run(tar, NULL, false) != 0) {
        fprintf(stderr, "purr: couldn't unpack the download\n");
        return 1;
    }
    swap s = {root, fresh, true};
    sys_list(fresh, swap_entry, &s);
    if (!s.ok) {
        fprintf(stderr, "purr: couldn't replace every file in %s; run `purr upgrade` again\n", root);
        return 1;
    }
    char *channel_file = path_join(root, "channel");
    sys_write_text(channel_file, channel);
    sys_remove_tree(work);
    printf("Upgraded purr %s -> %s (%s).\n", PURR_VERSION, next, channel);
    fflush(stdout);
    // The new purr updates the editor extension, in the editors that have it.
#ifdef _WIN32
    char *purr = path_join(root, "bin/purr.exe");
#else
    char *purr = path_join(root, "bin/purr");
#endif
    const char *const editors[] = {purr, "editors", "--update", NULL};
    sys_run(editors, NULL, false);
    free(purr);
    const char *notes = json_str(json_get(release, "html_url"));
    if (notes) printf("What's new: %s\n", notes);
    return 0;
}

void purr_check_for_update(const char *root)
{
    if (is_dev_build() || sys_env("PURR_NO_UPDATE_CHECK")) return;
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
    const char *channel = purr_channel(root);
    const json *release = releases ? newest(work, releases, channel) : NULL;
    if (release && purr_version_compare(purr_release_version(release), PURR_VERSION) > 0) {
        fprintf(stderr, "\npurr %s is out (you have %s): run `purr upgrade`\n", purr_release_version(release), PURR_VERSION);
    }
    sys_remove_tree(work);
}
