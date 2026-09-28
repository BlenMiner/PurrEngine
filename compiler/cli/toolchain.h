#pragma once

// Where the C compiler is, malloc'd, or NULL when it isn't installed.

// clang, searched like cmake/clang-toolchain.cmake: $LLVM_ROOT/bin, a standalone
// LLVM install, PATH, then Visual Studio's bundled clang. It builds web games
// too, with its WebAssembly target.
char *find_clang(void);

// The clang for web games: the same one, except on macOS, where Apple's clang
// has no WebAssembly target and Homebrew's LLVM does.
char *find_web_clang(void);

// wasm-ld, the WebAssembly linker: next to clang, on PATH, or Homebrew's lld.
char *find_wasm_ld(const char *clang);
