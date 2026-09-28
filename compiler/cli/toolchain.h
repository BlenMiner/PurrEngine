#pragma once

// Where the C compiler is, malloc'd, or NULL when it isn't installed.

// clang, searched like cmake/clang-toolchain.cmake: $LLVM_ROOT/bin, a standalone
// LLVM install, PATH, then Visual Studio's bundled clang. It builds web games
// too, with its WebAssembly target.
char *find_clang(void);

// wasm-ld, the WebAssembly linker clang runs: next to clang, or on PATH.
char *find_wasm_ld(const char *clang);
