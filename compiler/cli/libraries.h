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
} lib_platform;

enum {
    LIB_X64 = 1,
    LIB_ARM64 = 2,
    LIB_WASM32 = 4,
    LIB_OTHER_CPU = 8,
};

typedef struct lib_info {
    lib_platform platform;
    unsigned cpus; // LIB_X64 and the like: a universal macOS library has several
    bool dynamic;  // Loaded when the program starts (.dll, .so, .dylib), rather than linked in
} lib_info;

// Reads up to `n` bytes at `offset`, returning how many it read.
typedef size_t (*lib_read_fn)(void *user, uint64_t offset, void *buf, size_t n);

lib_info lib_identify(lib_read_fn read, void *user);
lib_info lib_identify_file(const char *path);

// Calls `found` with the name of every function a WebAssembly program imports
// from `module`. Web programs link with undefined functions allowed, which
// the page provides (the GL functions, from "env"), so a C function nothing
// defines turns up here. False if `wasm` isn't a program it can read.
typedef void (*wasm_import_fn)(void *user, const char *name, size_t len);
bool wasm_function_imports(const unsigned char *wasm, size_t size, const char *module, wasm_import_fn found, void *user);
