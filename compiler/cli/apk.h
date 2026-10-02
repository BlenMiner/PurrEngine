#pragma once

// Android apps as tide makes them, with no tool of Google's (no aapt2, no
// apksigner, no Java): an APK, which is a zip of the app's binary manifest and
// its library for each CPU, signed with APK Signature Scheme v2. The library
// is the program, which Android's NativeActivity loads (see
// platform/android/raylib/rcore_android_tide.c), so the app has no code or
// resources of its own.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APK_MAX_LIBS 4

typedef struct apk_desc {
    const char *package;      // The app's ID: "dev.tide.sand"
    const char *label;        // Its name under its icon
    const char *lib_name;     // lib<lib_name>.so, which the activity loads
    int version_code;         // Goes up with each release
    const char *version_name; // What people see: "1.0"
    int min_sdk;              // The oldest Android it runs on (API level)
    int target_sdk;           // The newest it was made for
    bool debuggable;          // Tools can attach to it
    int lib_count;
    const char *abis[APK_MAX_LIBS]; // "arm64-v8a", "x86_64"
    const char *libs[APK_MAX_LIBS]; // The library for each, on disk
    const char *icon;               // Its icon, a PNG on disk, or NULL for the system's
} apk_desc;

// The key an app is signed with: Android only takes an update signed with the
// same one.
typedef struct apk_key {
    uint8_t private_key[32]; // P-256
    uint8_t cert[512];       // Self-signed, DER
    size_t cert_size;
} apk_key;

// Reads the key at `path`, or makes one and writes it there.
bool apk_key_load(const char *path, apk_key *key, char *error, size_t error_size);

// The app's manifest, as Android reads it: binary XML. Returns its size, and
// sets *out to it (to free()), or 0.
size_t apk_manifest(const apk_desc *desc, uint8_t **out);

// The app's resource table (resources.arsc), which only names its icon: its
// size, and *out (to free()).
size_t apk_resources(const char *package, uint8_t **out);

// Writes the app to `path`, signed with `key`.
bool apk_write(const char *path, const apk_desc *desc, const apk_key *key, char *error, size_t error_size);
