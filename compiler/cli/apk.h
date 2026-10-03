#pragma once

// Android apps as tide makes them, with no tool of Google's (no aapt2, no
// apksigner, no Java): an APK, which is a zip of the app's binary manifest and
// its library for each CPU, signed with APK Signature Scheme v2; and an App
// Bundle, which Google Play takes and makes APKs of for each phone: the same
// files with the manifest and resources as protocol buffers, signed as a JAR.
// The library is the program, which Android's NativeActivity loads (see
// platform/android/activity.c), so the app has no code of its
// own, and its only resource is its icon. The key is sign.h's.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sign.h"

#define APK_MAX_LIBS 4

// A library the app's own needs: it goes beside it, where Android looks for
// it by its name when it loads the app's.
typedef struct apk_lib {
    const char *abi;  // "arm64-v8a"
    const char *name; // "libsteam_api.so": what the app's library asks for
    const char *path; // On disk
} apk_lib;

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
    int needed_count;
    const apk_lib *needed; // The libraries those need, each for one of the CPUs
} apk_desc;

// The app's manifest, as Android reads it: binary XML. Returns its size, and
// sets *out to it (to free()), or 0.
size_t apk_manifest(const apk_desc *desc, uint8_t **out);

// The app's resource table (resources.arsc), which only names its icon: its
// size, and *out (to free()).
size_t apk_resources(const char *package, uint8_t **out);

// The same two as an App Bundle has them: aapt2's protocol buffers
// (frameworks/base/tools/aapt2/Resources.proto), an XmlNode and a
// ResourceTable.
size_t apk_bundle_manifest(const apk_desc *desc, uint8_t **out);
size_t apk_bundle_resources(const char *package, uint8_t **out);

// Writes the app to `path`, signed with `key`.
bool apk_write(const char *path, const apk_desc *desc, const apk_key *key, char *error, size_t error_size);

// Writes the app as an App Bundle (.aab) to `path`, signed with `key`, for
// Google Play.
bool apk_bundle_write(const char *path, const apk_desc *desc, const apk_key *key, char *error, size_t error_size);
