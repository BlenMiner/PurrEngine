#pragma once

// Where the C compilers are, malloc'd, or NULL when they aren't installed.

// clang, searched like cmake/clang-toolchain.cmake: $LLVM_ROOT/bin, a standalone
// LLVM install, PATH, then Visual Studio's bundled clang.
char *find_clang(void);

// Emscripten's emcc (emcc.bat on Windows): $EMSDK, PATH, then the usual emsdk
// folders.
char *find_emcc(void);
