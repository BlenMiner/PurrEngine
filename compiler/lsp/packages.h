#pragma once

#include <stdbool.h>

// A game's packages (see docs/guide/packages.md): what its tide.packages file
// lists, and where each package's files are on this machine. tide and tidels
// share this; downloading is tide's (compiler/cli/fetch.c).
//
// tide.packages has a line each, and `#` starts a comment:
//
//   package Physics                      a package's own name, which is its namespace
//   tide 0.3                             the oldest tide it builds with
//   github.com/owner/repo 0123abcd...    a package from git, at that commit
//   github.com/owner/repo@dev 0123...    ...which `tide update` moves to the newest commit of dev
//   github.com/owner/repo@v1 0123...     ...or to the newest v1.x.y tag
//   github.com/owner/repo//sub 0123...   ...in a folder of the repository
//   ../shared                            a package in a folder on this machine
//
// A game's file lists every package it builds with, those its packages need
// included, and its line for one wins over theirs.

#define PACKAGES_FILE "tide.packages"

typedef struct package_line {
    int line;     // 1-based, in the file
    char *source; // "github.com/owner/repo", with "//sub/folder" for a folder in it; or a folder: "../shared"
    char *ref;    // What `tide update` follows: a branch, or a version (v1) for the newest such tag; NULL: the default branch
    char *commit; // Its hex digits; NULL for a folder, or a git line that has none yet
    bool local;   // A folder on this machine
} package_line;

typedef struct packages_file {
    char *path;   // The file
    char *folder; // The game's or the package's folder, absolute and ending in '/'
    bool exists;  // The folder has a tide.packages
    char *name;   // `package Physics`: the package this folder is; NULL for a game
    int tide[3];  // `tide 0.3`: the oldest tide it builds with; zeros for any
    int tide_line;
    package_line *lines;
    int count;
} packages_file;

// What's wrong, said to tide's user or dropped by tidels: `where` is a file
// with `line` in it, or a folder (`line` 0). `note`, which says what to do
// about it, may be NULL.
typedef void (*packages_report_fn)(void *user, const char *where, int line, const char *message, const char *note);

// Reads `folder`'s tide.packages. A folder without one is a game with no
// packages. False after reporting what's wrong with the file; what could be
// read is in `out` either way.
bool packages_read(const char *folder, packages_file *out, packages_report_fn report, void *user);
void packages_free_file(packages_file *f);

// One of a game's packages, ready to compile.
typedef struct package {
    char *name;               // Its `package` line's: its namespace
    char *folder;             // Its files, absolute and ending in '/'
    const package_line *line; // The game's line for it
} package;

typedef struct game_packages {
    packages_file file; // The game's tide.packages
    package *items;     // In the order they compile: each after the packages it needs, then by name
    int count;
} game_packages;

// Gets a git package that isn't on this machine yet into `dir`
// (packages_repo_dir), reporting what went wrong if it can't.
typedef bool (*packages_fetch_fn)(void *user, const package_line *line, const char *dir);

// Reads the tide.packages of the game (or package) in `folder` and finds its
// packages, with `fetch` getting those from git that aren't here yet (NULL:
// they're left out). Checks that each is a package, that this tide is new
// enough for it, and that the game lists what it needs. False after reporting
// what's wrong; the packages it found are in `out` either way.
bool packages_resolve(const char *folder, game_packages *out, packages_fetch_fn fetch, packages_report_fn report,
                      void *user);
void packages_free(game_packages *g);

// A package that a package needs, which the game's tide.packages doesn't list.
typedef struct package_need {
    char *line; // The line for it, as the game's tide.packages writes it
    char *by;   // The package that needs it
} package_need;

// What the packages of the game in `folder` need that its tide.packages
// doesn't list, each once, with `fetch` getting packages that aren't here yet
// (to read what they need). Returns how many, into `out` (malloc'd).
int packages_missing(const char *folder, packages_fetch_fn fetch, void *user, package_need **out);
void packages_free_needs(package_need *needs, int count);

// Where packages from git are kept, each commit once: $TIDE_PACKAGES, or else
// %LOCALAPPDATA%/Tide/packages on Windows and ~/.tide/packages elsewhere.
// Ends in '/'; malloc'd.
char *packages_cache(void);

// Where a git line's repository is, at its commit: <cache>/<host>/<owner>/
// <repo>/<commit>/. malloc'd; NULL for a folder's line or one with no commit.
char *packages_repo_dir(const package_line *line);

// A package's own folder, ending in '/': a local line's, relative to `base`
// (the folder of the file it's in), or the repository's, or the folder in it.
// malloc'd; NULL for a git line with no commit.
char *packages_folder(const package_line *line, const char *base);

// The repository part of a git source: "github.com/owner/repo//sub" ->
// "github.com/owner/repo". malloc'd.
char *packages_repo(const char *source);

// What someone gives `tide add` as a line's source and ref: a URL
// (https://github.com/owner/repo.git, git@github.com:owner/repo) or a source
// as tide.packages writes it, with @ref after it or not. A folder stays as
// written. False if it's neither.
bool packages_parse_source(const char *text, char **source, char **ref);

// Whether a source is a folder on this machine rather than a git repository.
bool packages_is_local(const char *source);

// `path` (absolute) relative to the folder `from` (absolute, ending in '/'),
// like "../shared"; or `path` itself where it can't be. malloc'd.
char *packages_relative(const char *from, const char *path);
