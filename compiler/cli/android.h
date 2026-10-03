#pragma once

// Android for tide (see AGENTS.md, Android builds): Google's NDK, which games
// build against, and adb, which puts them on a phone. Each is found where
// Android's own tools put it, or else fetched once from Google, after asking,
// into <root>/android: only what tide needs of it.

#include <stdbool.h>

#define ANDROID_ABIS 2
#define ANDROID_API "29" // Android 10, the oldest games run on (cmake/android-toolchain.cmake says why)

extern const char *const android_abis[ANDROID_ABIS];   // "arm64-v8a", "x86_64": as Android names them
extern const char *const android_arches[ANDROID_ABIS]; // "aarch64", "x86_64": as clang does

typedef struct android_ndk {
    char *sysroot;                // Android's headers and libraries
    char *builtins[ANDROID_ABIS]; // The compiler runtime, for each
} android_ndk;

// Finds the NDK, or fetches it. False after saying why it can't.
bool android_find_ndk(const char *root, android_ndk *ndk);

// adb: its path, or NULL after saying why it can't find or fetch it.
char *android_find_adb(const char *root);

// The key tide signs apps with on this machine (sign.h): the file
// TIDE_ANDROID_KEY names, else <home>/.android/tide.pem, where Android's own
// tools keep theirs. An app updates only with the key it was installed with.
char *android_key_path(void);
