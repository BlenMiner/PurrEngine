#include "packages_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fetch.h"
#include "packages.h"
#include "sys.h"

static char *dup(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (!out) abort();
    memcpy(out, s, n);
    return out;
}

static bool same_text(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        if (!*a) return true;
    }
}

static bool same_path(const char *a, const char *b)
{
#ifdef _WIN32
    return same_text(a, b); // Windows paths ignore case
#else
    return strcmp(a, b) == 0;
#endif
}

static void report(void *user, const char *where, const int line, const char *message, const char *note)
{
    (void)user;
    if (line > 0) fprintf(stderr, "%s:%d: error: %s\n", where, line, message);
    else fprintf(stderr, "%s: error: %s\n", where, message);
    if (note) fprintf(stderr, "  = note: %s\n", note);
}

// ---------------------------------------------------------------------------
// The file as text, so what's around the lines that change (comments, order,
// blank lines) stays as it is.

typedef struct packages_text {
    char *path;
    char *text;
    const char *newline; // "\r\n" in a file that has them
} packages_text;

static packages_text load(const char *folder)
{
    packages_text t = {path_join(folder, PACKAGES_FILE), NULL, "\n"};
    t.text = sys_read_file(t.path, NULL);
    if (!t.text) t.text = dup("");
    if (strstr(t.text, "\r\n")) t.newline = "\r\n";
    return t;
}

static bool save(packages_text *t)
{
    const bool ok = sys_write_text(t->path, t->text);
    if (!ok) fprintf(stderr, "tide: can't write %s\n", t->path);
    free(t->path);
    free(t->text);
    return ok;
}

static void append(packages_text *t, const char *line)
{
    const size_t n = strlen(t->text);
    const bool ends = n == 0 || t->text[n - 1] == '\n';
    const size_t size = n + strlen(line) + 8;
    char *out = malloc(size);
    if (!out) abort();
    snprintf(out, size, "%s%s%s%s", t->text, ends ? "" : t->newline, line, t->newline);
    free(t->text);
    t->text = out;
}

static bool is_space(const char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Puts `commit` on line `number`: in place of its second word, before any
// comment, or after its first word if it has none.
static void set_commit(packages_text *t, const int number, const char *commit)
{
    char *line = t->text;
    for (int i = 1; i < number && line; i++) {
        line = strchr(line, '\n');
        if (line) line++;
    }
    if (!line) return;
    size_t end = strcspn(line, "\n");
    for (size_t i = 0; i < end; i++) {
        if (line[i] == '#' && (i == 0 || is_space(line[i - 1]))) {
            end = i;
            break;
        }
    }
    size_t first = 0;
    while (first < end && is_space(line[first])) first++;
    while (first < end && !is_space(line[first])) first++;
    size_t second = first;
    while (second < end && is_space(line[second])) second++;
    size_t second_end = second;
    while (second_end < end && !is_space(line[second_end])) second_end++;
    const bool has_second = second_end > second;
    const size_t from = (size_t)(line - t->text) + (has_second ? second : first);
    const size_t to = (size_t)(line - t->text) + (has_second ? second_end : first);
    const size_t size = strlen(t->text) + strlen(commit) + 2;
    char *out = malloc(size);
    if (!out) abort();
    snprintf(out, size, "%.*s%s%s%s", (int)from, t->text, has_second ? "" : " ", commit, t->text + to);
    free(t->text);
    t->text = out;
}

// ---------------------------------------------------------------------------

// What to call a line's package: its name, when it's here to read, or else
// its source. malloc'd.
static char *package_name(const package_line *line, const char *folder)
{
    char *dir = packages_folder(line, folder);
    char *name = NULL;
    if (dir) {
        packages_file pf;
        packages_read(dir, &pf, NULL, NULL);
        if (pf.name) name = dup(pf.name);
        packages_free_file(&pf);
        free(dir);
    }
    return name ? name : dup(line->source);
}

// Adds the lines of what the game's packages need that it doesn't list yet,
// and then what those need, and so on. False after saying what went wrong.
static bool add_needed(const char *folder)
{
    char **added = NULL;
    int added_count = 0;
    bool ok = true;
    for (bool more = true; more && ok;) {
        package_need *needs = NULL;
        const int n = packages_missing(folder, fetch_package, NULL, &needs);
        more = n > 0;
        packages_text t = load(folder);
        for (int i = 0; i < n && ok; i++) {
            for (int k = 0; k < added_count && ok; k++) {
                if (strcmp(added[k], needs[i].line) != 0) continue;
                // Added already, and still missing: it isn't the package they need.
                fprintf(stderr, "tide: %s needs %s, which doesn't seem to be it\n", needs[i].by, needs[i].line);
                ok = false;
            }
            if (!ok) break;
            append(&t, needs[i].line);
            printf("tide: added %s, which %s needs\n", needs[i].line, needs[i].by);
            added = realloc(added, sizeof(char *) * (size_t)(added_count + 1));
            if (!added) abort();
            added[added_count++] = dup(needs[i].line);
        }
        if (more && !save(&t)) ok = false;
        if (!more) {
            free(t.path);
            free(t.text);
        }
        packages_free_needs(needs, n);
    }
    for (int i = 0; i < added_count; i++) free(added[i]);
    free(added);
    return ok;
}

// The ref's own name: main for refs/heads/main, v1.4.2 for refs/tags/v1.4.2.
static const char *short_ref(const char *name)
{
    if (strncmp(name, "refs/heads/", 11) == 0) return name + 11;
    if (strncmp(name, "refs/tags/", 10) == 0) return name + 10;
    return name;
}

// The newest commit of what `line` follows, saying what's wrong if there's none.
static const char *newest_commit(const git_refs *refs, const char *source, const char *ref, const char **found)
{
    const char *commit = git_refs_resolve(refs, ref, found);
    if (commit) return commit;
    char *repo = packages_repo(source);
    if (ref) {
        fprintf(stderr, "tide: %s has no branch or tag called %s\n", repo, ref);
        fprintf(stderr, "  = note: after @ goes a branch, like @main, or a version, like @v1 for the newest v1.x.y tag\n");
    } else {
        fprintf(stderr, "tide: %s doesn't say which branch is its default\n", repo);
        fprintf(stderr, "  = note: follow a branch by naming it: %s@main\n", source);
    }
    free(repo);
    return NULL;
}

int tide_add(const char *folder, const char *what)
{
    char *source = NULL;
    char *ref = NULL;
    if (!packages_parse_source(what, &source, &ref)) {
        fprintf(stderr, "tide: '%s' isn't a package's source\n", what);
        fprintf(stderr, "  = note: give a git repository, like github.com/owner/repo or https://github.com/owner/repo.git, "
                        "with @branch or @v1 after it to follow a branch or a version; or a folder, like ../shared\n");
        return 2;
    }
    packages_file game;
    if (!packages_read(folder, &game, report, NULL)) return 1;
    package_line line = {0, source, ref, NULL, packages_is_local(source)};
    for (int i = 0; i < game.count; i++) {
        const package_line *other = &game.lines[i];
        bool same = other->local == line.local;
        if (same && !line.local) same = strcmp(other->source, source) == 0;
        if (same && line.local) {
            char *a = packages_folder(other, game.folder);
            char *b = packages_folder(&line, game.folder);
            same = same_path(a, b);
            free(a);
            free(b);
        }
        if (!same) continue;
        char *name = package_name(other, game.folder);
        printf("tide: %s is in tide.packages already, on line %d\n", name, other->line);
        if (!other->local) printf("  = note: `tide update %s` moves it to the newest commit\n", name);
        free(name);
        return 0;
    }

    git_refs refs = {0};
    const char *found = NULL;
    if (!line.local) {
        char *repo = packages_repo(source);
        const bool listed = fetch_refs(repo, &refs);
        free(repo);
        if (!listed) return 1;
        const char *commit = newest_commit(&refs, source, ref, &found);
        if (!commit) return 1;
        line.commit = dup(commit);
        char *repo_dir = packages_repo_dir(&line);
        const bool here = sys_is_dir(repo_dir) || fetch_package(NULL, &line, repo_dir);
        free(repo_dir);
        if (!here) return 1;
    }
    char *dir = packages_folder(&line, game.folder);
    if (!sys_is_dir(dir)) {
        fprintf(stderr, "tide: %s isn't a folder\n", line.local ? source : dir);
        return 1;
    }
    packages_file pf;
    packages_read(dir, &pf, report, NULL);
    if (!pf.name) {
        fprintf(stderr, "tide: %s isn't a package: %s\n", source,
                pf.exists ? "its tide.packages has no `package` line" : "it has no tide.packages");
        fprintf(stderr, "  = note: a package's tide.packages starts with its name, which is its namespace: "
                        "`package Physics`\n");
        return 1;
    }

    char text[1024];
    if (line.local) snprintf(text, sizeof text, "%s", source);
    else snprintf(text, sizeof text, "%s%s%s %s", source, ref ? "@" : "", ref ? ref : "", line.commit);
    packages_text t = load(folder);
    append(&t, text);
    if (!save(&t)) return 1;
    if (line.local) printf("tide: added %s, from %s\n", pf.name, source);
    else printf("tide: added %s, at %.7s (%s)\n", pf.name, line.commit, short_ref(found));
    packages_free_file(&pf);
    git_refs_free(&refs);
    free(dir);
    return add_needed(folder) ? 0 : 1;
}

// Whether `name` names the package of `line`: by its own name, its
// repository's or its folder's, or its source.
static bool names_line(const char *name, const package_line *line, const char *folder)
{
    char *own = package_name(line, folder);
    const char *last = strrchr(line->source, '/');
    const bool match = same_text(name, own) || same_text(name, line->source) || (last && same_text(name, last + 1));
    free(own);
    return match;
}

int tide_update(const char *folder, const char *const *names, const int count)
{
    packages_file game;
    if (!packages_read(folder, &game, report, NULL)) return 1;
    if (!game.exists) {
        fprintf(stderr, "tide: there's no tide.packages in %s\n", game.folder);
        fprintf(stderr, "  = note: `tide add <source>` starts one\n");
        return 1;
    }
    bool *chosen = calloc((size_t)game.count + 1, sizeof(bool));
    if (!chosen) abort();
    for (int i = 0; i < game.count && count == 0; i++) chosen[i] = !game.lines[i].local;
    for (int n = 0; n < count; n++) {
        bool any = false;
        for (int i = 0; i < game.count; i++) {
            if (!names_line(names[n], &game.lines[i], game.folder)) continue;
            chosen[i] = any = true;
        }
        if (any) continue;
        fprintf(stderr, "tide: tide.packages has no package called %s\n", names[n]);
        if (game.count > 0) {
            fprintf(stderr, "  = note: it has ");
            for (int i = 0; i < game.count; i++) {
                char *name = package_name(&game.lines[i], game.folder);
                fprintf(stderr, "%s%s", i == 0 ? "" : i + 1 == game.count ? " and " : ", ", name);
                free(name);
            }
            fprintf(stderr, "\n");
        }
        return 1;
    }

    packages_text t = load(folder);
    bool changed = false;
    bool failed = false;
    bool any_git = false;
    for (int i = 0; i < game.count; i++) {
        const package_line *line = &game.lines[i];
        if (!chosen[i]) continue;
        if (line->local) {
            char *name = package_name(line, game.folder);
            printf("tide: %s is a folder on this machine, so it's always up to date\n", name);
            free(name);
            continue;
        }
        any_git = true;
        char *repo = packages_repo(line->source);
        git_refs refs;
        const char *found = NULL;
        const char *commit = fetch_refs(repo, &refs) ? newest_commit(&refs, line->source, line->ref, &found) : NULL;
        // The new commit, here to build with, and to read its name from
        package_line now = *line;
        now.commit = (char *)commit;
        char *repo_dir = commit ? packages_repo_dir(&now) : NULL;
        if (repo_dir && !sys_is_dir(repo_dir) && !fetch_package(NULL, &now, repo_dir)) commit = NULL;
        free(repo_dir);
        char *name = package_name(commit ? &now : line, game.folder);
        if (!commit) {
            failed = true;
        } else if (line->commit && strcmp(line->commit, commit) == 0) {
            printf("tide: %s is up to date, at %.7s (%s)\n", name, commit, short_ref(found));
        } else {
            set_commit(&t, line->line, commit);
            changed = true;
            if (line->commit) printf("tide: %s %.7s -> %.7s (%s)\n", name, line->commit, commit, short_ref(found));
            else printf("tide: %s is at %.7s (%s)\n", name, commit, short_ref(found));
        }
        git_refs_free(&refs);
        free(repo);
        free(name);
    }
    if (count == 0 && !any_git) printf("tide: tide.packages has no packages from git to update\n");
    free(chosen);
    if (changed && !save(&t)) return 1;
    if (!changed) {
        free(t.path);
        free(t.text);
    }
    packages_free_file(&game);
    return add_needed(folder) && !failed ? 0 : 1;
}
