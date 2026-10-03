#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Prebuilt libraries in a game's folder: which platform and CPU each was
// built for, read from its contents, since a folder holds every platform's
// and names don't say (`.a` is MinGW's, Linux's, macOS's and the web's).
// A build links the ones built for its target (see AGENTS.md).

typedef enum lib_platform {
    LIB_UNKNOWN,
    LIB_WINDOWS, // COFF objects and import libraries, or a PE .dll
    LIB_LINUX,   // ELF
    LIB_MACOS,   // Mach-O, maybe universal
    LIB_WEB,     // WebAssembly objects
    LIB_ANDROID, // ELF that says it's Android's
} lib_platform;

// Linux's libraries and Android's are both ELF, for the same CPUs. A shared
// library says which it is: Android's toolchain puts a note in each one it
// links (.note.android.ident, from the NDK's crtbegin_so.o: the Android it was
// built for and the NDK that built it), and so do the others that link for
// Android (Go's). A static library doesn't: the objects clang makes for the
// two are the same, so an ELF one is taken for Linux's.

enum {
    LIB_X64 = 1,
    LIB_ARM64 = 2,
    LIB_WASM32 = 4,
    LIB_OTHER_CPU = 8,
};

#define LIB_NAME_MAX 128

typedef struct lib_info {
    lib_platform platform;
    unsigned cpus; // LIB_X64 and the like: a universal macOS library has several
    bool dynamic;  // Loaded when the program starts (.dll, .so, .dylib), rather than linked in
    // A 64-bit ELF shared library's:
    uint64_t page_size;      // What its segments are aligned to: it loads where pages are no bigger. 0 if it has none
    char name[LIB_NAME_MAX]; // Its soname, which what links to it asks for it by; "" for none: its file's name then
} lib_info;

// Reads up to `n` bytes at `offset`, returning how many it read.
typedef size_t (*lib_read_fn)(void *user, uint64_t offset, void *buf, size_t n);

lib_info lib_identify(lib_read_fn read, void *user);
lib_info lib_identify_file(const char *path);

// Calls `found` with the name of every function a WebAssembly program or
// object imports from `module`. An object imports from "env" the functions it
// calls and doesn't define. False if `wasm` isn't one it can read.
typedef void (*wasm_import_fn)(void *user, const char *name, size_t len);
bool wasm_function_imports(const unsigned char *wasm, size_t size, const char *module, wasm_import_fn found, void *user);

// The same for every WebAssembly object of a static library (.a): what the
// library leaves for others to define. Web programs may leave undefined what
// the platform layer's libraries do, which the page defines (GL's functions).
// False if `archive` isn't an archive.
bool wasm_archive_imports(const unsigned char *archive, size_t size, const char *module, wasm_import_fn found,
                          void *user);
