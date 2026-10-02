#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "packages.h"

// Packages from git, over HTTPS with curl and tar, as tide upgrade gets tide:
// a commit as its host's archive of it, and the branches and tags from git's
// own list of them (info/refs), which every host serves the same way. No git
// needed.

// Downloads the repository of a git line, at its commit, into `dir`
// (packages_repo_dir). A packages_fetch_fn, for packages_resolve: false after
// saying what went wrong.
bool fetch_package(void *user, const package_line *line, const char *dir);

typedef struct git_ref {
    char *name;      // refs/heads/main, refs/tags/v1.2.0
    char commit[65]; // For a tag, the commit it's on
} git_ref;

typedef struct git_refs {
    git_ref *items;
    int count;
    char *head; // The default branch, like refs/heads/main; NULL if the host didn't say
} git_refs;

// The branches and tags of `repo` ("github.com/owner/repo"). False after
// saying what went wrong.
bool fetch_refs(const char *repo, git_refs *out);

// Reads git's list of refs, as info/refs?service=git-upload-pack gives it.
// False if it isn't one.
bool git_refs_parse(const char *data, size_t len, git_refs *out);
void git_refs_free(git_refs *refs);

// The commit `ref` is: NULL for the default branch; a version (v1, v1.2,
// 1.2.3) for the newest tag of it, leaving out pre-releases; anything else a
// branch, or else a tag, of that name. `name` gets the ref it found, like
// refs/tags/v1.4.2. NULL if there's none.
const char *git_refs_resolve(const git_refs *refs, const char *ref, const char **name);
