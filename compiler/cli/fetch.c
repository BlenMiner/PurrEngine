#include "fetch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"

static char *dup_n(const char *s, const size_t n)
{
    char *out = malloc(n + 1);
    if (!out) abort();
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static bool host_is(const char *repo, const char *host)
{
    const size_t n = strlen(host);
    return strncmp(repo, host, n) == 0 && repo[n] == '/';
}

// For tests: TIDE_GIT_MIRROR, a folder that stands in for every host, where
// <mirror>/<repo>/refs is a repository's list of branches and tags, and
// <mirror>/<repo>/<commit>.tar.gz an archive of a commit. NULL without it.
static char *mirror_url(const char *repo, const char *file)
{
    const char *mirror = sys_env("TIDE_GIT_MIRROR");
    if (!mirror) return NULL;
    char url[2048];
    snprintf(url, sizeof url, "file://%s%s/%s/%s", mirror[0] == '/' ? "" : "/", mirror, repo, file);
    return dup_n(url, strlen(url));
}

// The host's URL for an archive of `repo` at `commit`, malloc'd; NULL for a
// host tide doesn't know.
static char *archive_url(const char *repo, const char *commit)
{
    const char *name = strrchr(repo, '/') + 1;
    char url[2048];
    snprintf(url, sizeof url, "%s.tar.gz", commit);
    char *mirrored = mirror_url(repo, url);
    if (mirrored) return mirrored;
    if (host_is(repo, "github.com") || host_is(repo, "codeberg.org")) { // Codeberg's is Forgejo's
        snprintf(url, sizeof url, "https://%s/archive/%s.tar.gz", repo, commit);
    } else if (host_is(repo, "gitlab.com")) {
        snprintf(url, sizeof url, "https://%s/-/archive/%s/%s-%s.tar.gz", repo, commit, name, commit);
    } else if (host_is(repo, "bitbucket.org")) {
        snprintf(url, sizeof url, "https://%s/get/%s.tar.gz", repo, commit);
    } else {
        return NULL;
    }
    return dup_n(url, strlen(url));
}

// A folder of its own for this tide's downloads, next to the packages, so a
// finished one moves into place with a rename. Made anew.
static char *download_dir(void)
{
    char *cache = packages_cache();
    char name[64];
    snprintf(name, sizeof name, ".downloads/%u", (unsigned)sys_pid());
    char *work = path_join(cache, name);
    free(cache);
    sys_remove_tree(work);
    sys_mkdirs(work);
    return work;
}

static bool curl(const char *url, const char *path, const char *timeout)
{
    const char *const argv[] = {sys_tool("curl"), "-fsSL", "--retry", "2", "-m", timeout, "-o", path, url, NULL};
    return sys_run(argv, NULL, false) == 0;
}

bool fetch_package(void *user, const package_line *line, const char *dir)
{
    (void)user;
    char *repo = packages_repo(line->source);
    char *url = archive_url(repo, line->commit);
    if (!url) {
        fprintf(stderr, "tide: tide can't download packages from %.*s yet\n", (int)strcspn(repo, "/"), repo);
        fprintf(stderr, "  = note: packages come from github.com, gitlab.com, codeberg.org and bitbucket.org, or "
                        "from a folder on this machine\n");
        free(repo);
        return false;
    }
    printf("tide: downloading %s at %.10s\n", repo, line->commit);
    fflush(stdout);
    char *work = download_dir();
    char *archive = path_join(work, "archive.tar.gz");
    char *files = path_join(work, "files");
    // The archive holds one folder, the repository's: its files go straight in.
    const char *const tar[] = {sys_tool("tar"), "-xzf", archive, "--strip-components=1", "-C", files, NULL};
    bool ok = curl(url, archive, "600");
    if (!ok) {
        fprintf(stderr, "tide: couldn't download %s at %s\n", repo, line->commit);
        fprintf(stderr, "  = note: check that the repository is public and has that commit\n");
    } else if (!sys_mkdirs(files) || sys_run(tar, NULL, false) != 0) {
        fprintf(stderr, "tide: couldn't unpack %s at %s\n", repo, line->commit);
        ok = false;
    }
    if (ok) {
        char *target = dup_n(dir, strlen(dir));
        const size_t n = strlen(target);
        if (n > 1 && target[n - 1] == '/') target[n - 1] = '\0';
        char *parent = path_dir(target);
        sys_mkdirs(parent);
        // Another tide may have got it meanwhile.
        if (!sys_rename(files, target) && !sys_is_dir(target)) {
            fprintf(stderr, "tide: can't write %s\n", target);
            ok = false;
        }
        free(parent);
        free(target);
    }
    sys_remove_tree(work);
    free(work);
    free(archive);
    free(files);
    free(url);
    free(repo);
    return ok;
}

// ---------------------------------------------------------------------------
// Branches and tags

void git_refs_free(git_refs *refs)
{
    for (int i = 0; i < refs->count; i++) free(refs->items[i].name);
    free(refs->items);
    free(refs->head);
    memset(refs, 0, sizeof *refs);
}

static int hex_value(const char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_hash(const char *text, const size_t n)
{
    if (n != 40 && n != 64) return false;
    for (size_t i = 0; i < n; i++) {
        if (hex_value(text[i]) < 0) return false;
    }
    return true;
}

static void add_ref(git_refs *refs, const char *name, const size_t name_len, const char *hash, const size_t hash_len)
{
    refs->items = realloc(refs->items, sizeof(git_ref) * (size_t)(refs->count + 1));
    if (!refs->items) abort();
    git_ref *r = &refs->items[refs->count++];
    r->name = dup_n(name, name_len);
    for (size_t i = 0; i < hash_len; i++) r->commit[i] = (char)tolower((unsigned char)hash[i]);
    r->commit[hash_len] = '\0';
}

// Git's packet lines: each starts with its length in 4 hex digits, those
// included, and 0000 ends a section. The first section names the service;
// the next lists "<hash> <ref>", the first with "\0" and what the server can
// do after it, which says where HEAD points. An annotated tag is followed by
// "<hash> <tag>^{}", the commit it's on.
bool git_refs_parse(const char *data, const size_t len, git_refs *out)
{
    memset(out, 0, sizeof *out);
    size_t pos = 0;
    while (pos + 4 <= len) {
        int n = 0;
        for (int i = 0; i < 4; i++) {
            const int v = hex_value(data[pos + (size_t)i]);
            if (v < 0) {
                git_refs_free(out);
                return false;
            }
            n = n * 16 + v;
        }
        if (n == 0) {
            pos += 4;
            continue;
        }
        if (n < 4 || pos + (size_t)n > len) {
            git_refs_free(out);
            return false;
        }
        const char *line = data + pos + 4;
        size_t line_len = (size_t)n - 4;
        pos += (size_t)n;
        if (line_len > 0 && line[line_len - 1] == '\n') line_len--;
        if (line_len >= 10 && memcmp(line, "# service=", 10) == 0) continue;
        const char *space = memchr(line, ' ', line_len);
        if (!space || !is_hash(line, (size_t)(space - line))) continue;
        const size_t hash_len = (size_t)(space - line);
        const char *name = space + 1;
        const char *nul = memchr(name, '\0', line_len - hash_len - 1);
        const size_t name_len = nul ? (size_t)(nul - name) : line_len - hash_len - 1;
        if (nul) {
            // symref=HEAD:refs/heads/main among what it can do
            const char *caps = nul + 1;
            const size_t caps_len = line_len - (size_t)(caps - line);
            const char *key = "symref=HEAD:";
            for (size_t i = 0; i + strlen(key) <= caps_len; i++) {
                if ((i == 0 || caps[i - 1] == ' ') && memcmp(caps + i, key, strlen(key)) == 0) {
                    const char *value = caps + i + strlen(key);
                    size_t value_len = 0;
                    while (value + value_len < caps + caps_len && value[value_len] != ' ') value_len++;
                    free(out->head);
                    out->head = dup_n(value, value_len);
                    break;
                }
            }
        }
        if (name_len >= 3 && memcmp(name + name_len - 3, "^{}", 3) == 0) {
            // The commit of the tag listed just before
            if (out->count > 0 && strlen(out->items[out->count - 1].name) == name_len - 3
                && memcmp(out->items[out->count - 1].name, name, name_len - 3) == 0) {
                git_ref *tag = &out->items[out->count - 1];
                for (size_t i = 0; i < hash_len; i++) tag->commit[i] = (char)tolower((unsigned char)line[i]);
                tag->commit[hash_len] = '\0';
            }
            continue;
        }
        add_ref(out, name, name_len, line, hash_len);
    }
    return out->count > 0;
}

bool fetch_refs(const char *repo, git_refs *out)
{
    memset(out, 0, sizeof *out);
    char url[2048];
    snprintf(url, sizeof url, "https://%s.git/info/refs?service=git-upload-pack", repo);
    char *mirrored = mirror_url(repo, "refs");
    if (mirrored) snprintf(url, sizeof url, "%s", mirrored);
    free(mirrored);
    char *work = download_dir();
    char *path = path_join(work, "refs");
    size_t len = 0;
    char *data = curl(url, path, "60") ? sys_read_file(path, &len) : NULL;
    const bool ok = data && git_refs_parse(data, len, out);
    if (!ok) {
        fprintf(stderr, "tide: couldn't get the branches and tags of %s\n", repo);
        fprintf(stderr, "  = note: check that it's a public git repository: %s\n", url);
    }
    free(data);
    free(path);
    sys_remove_tree(work);
    free(work);
    return ok;
}

// v1, v1.2, 1.2.3: up to three numbers, with nothing after them. `parts`
// gets how many there are.
static bool parse_version(const char *text, int out[3], int *parts)
{
    if (*text == 'v' || *text == 'V') text++;
    *parts = 0;
    out[0] = out[1] = out[2] = 0;
    while (*parts < 3) {
        if (!isdigit((unsigned char)*text)) return false;
        int v = 0;
        while (isdigit((unsigned char)*text) && v < 100000000) v = v * 10 + (*text++ - '0');
        out[(*parts)++] = v;
        if (*text == '\0') return true;
        if (*text != '.') return false;
        text++;
    }
    return false;
}

static const git_ref *find_ref(const git_refs *refs, const char *name)
{
    for (int i = 0; i < refs->count; i++) {
        if (strcmp(refs->items[i].name, name) == 0) return &refs->items[i];
    }
    return NULL;
}

const char *git_refs_resolve(const git_refs *refs, const char *ref, const char **name)
{
    const git_ref *found = NULL;
    if (!ref) {
        found = refs->head ? find_ref(refs, refs->head) : NULL;
        // A host that doesn't say: the branch HEAD is on, by its commit.
        const git_ref *head = found ? NULL : find_ref(refs, "HEAD");
        for (int i = 0; head && i < refs->count && !found; i++) {
            const git_ref *r = &refs->items[i];
            if (strncmp(r->name, "refs/heads/", 11) == 0 && strcmp(r->commit, head->commit) == 0) found = r;
        }
        if (!found) found = head;
    } else {
        int want[3];
        int parts = 0;
        if (parse_version(ref, want, &parts)) {
            // The newest tag that starts with those numbers
            int best[3] = {-1, -1, -1};
            for (int i = 0; i < refs->count; i++) {
                const char *tag = refs->items[i].name;
                if (strncmp(tag, "refs/tags/", 10) != 0) continue;
                int have[3];
                int have_parts = 0;
                if (!parse_version(tag + 10, have, &have_parts) || have_parts < parts) continue;
                bool match = true;
                for (int k = 0; k < parts && match; k++) match = have[k] == want[k];
                if (!match) continue;
                bool newer = false;
                for (int k = 0; k < 3; k++) {
                    if (have[k] != best[k]) {
                        newer = have[k] > best[k];
                        break;
                    }
                }
                if (newer) {
                    memcpy(best, have, sizeof best);
                    found = &refs->items[i];
                }
            }
        }
        char full[512];
        if (!found) {
            snprintf(full, sizeof full, "refs/heads/%s", ref);
            found = find_ref(refs, full);
        }
        if (!found) {
            snprintf(full, sizeof full, "refs/tags/%s", ref);
            found = find_ref(refs, full);
        }
        if (!found) found = find_ref(refs, ref);
    }
    if (name) *name = found ? found->name : NULL;
    return found ? found->commit : NULL;
}
