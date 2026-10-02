#include "android.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define stdin_is_terminal() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define stdin_is_terminal() isatty(0)
#endif

#include "rtc.h" // SHA-1, the platform layer's, for Google's checksums
#include "sys.h"
#include "unzip.h"

const char *const android_abis[ANDROID_ABIS] = {"arm64-v8a", "x86_64"};
const char *const android_arches[ANDROID_ABIS] = {"aarch64", "x86_64"};

// What tide fetches, pinned: Google's repository lists them with their SHA-1s
// (https://dl.google.com/android/repository/repository2-3.xml).
#define GOOGLE "https://dl.google.com/android/repository/"
#ifdef _WIN32
#define HOST "windows-x86_64"
#define NDK_ZIP "android-ndk-r30-windows.zip"
#define NDK_SHA1 "9bf167a1985fa7d4a036186b78f702eab9179408"
#define TOOLS_ZIP "platform-tools_r37.0.1-win.zip"
#define TOOLS_SHA1 "e03e78b1d80b396f1c3358e31251cb31740e1110"
#define EXE ".exe"
#elif defined(__APPLE__)
#define HOST "darwin-x86_64" // Both of Apple's CPUs
#define NDK_ZIP "android-ndk-r30-darwin.zip"
#define NDK_SHA1 "c060be96767eefbb8e0a27796d6f43115fc1a0c4"
#define TOOLS_ZIP "platform-tools_r37.0.1-darwin.zip"
#define TOOLS_SHA1 "6ae73f4de6452dc57e62ec02b68eed92a4c21661"
#define EXE ""
#else
#define HOST "linux-x86_64"
#define NDK_ZIP "android-ndk-r30-linux.zip"
#define NDK_SHA1 "5107f898313790e449e87eee2183d9a20602dee9"
#define TOOLS_ZIP "platform-tools_r37.0.1-linux.zip"
#define TOOLS_SHA1 "477254aa5f903c15cf51001717bdf347fb6b53e0"
#define EXE ""
#endif

static char *home(void)
{
#ifdef _WIN32
    const char *dir = sys_env("USERPROFILE");
#else
    const char *dir = sys_env("HOME");
#endif
    return path_absolute(dir ? dir : ".");
}

// Android's SDK folders: where its variables say, and where Android Studio puts it.
static int sdk_folders(char **out)
{
    int n = 0;
    const char *vars[] = {"ANDROID_HOME", "ANDROID_SDK_ROOT"};
    for (int i = 0; i < 2; i++) {
        if (sys_env(vars[i])) out[n++] = path_absolute(sys_env(vars[i]));
    }
#ifdef _WIN32
    if (sys_env("LOCALAPPDATA")) out[n++] = path_join(sys_env("LOCALAPPDATA"), "Android/Sdk");
#elif defined(__APPLE__)
    char *h = home();
    out[n++] = path_join(h, "Library/Android/sdk");
    free(h);
#else
    char *h = home();
    out[n++] = path_join(h, "Android/Sdk");
    free(h);
#endif
    return n;
}

// ---------------------------------------------------------------------------
// Fetching from Google, once its license is taken

static bool agreed(const char *root, const char *what, const char *size)
{
    char *mark = path_join(root, "android/license-accepted");
    const bool before = sys_exists(mark) || sys_env("TIDE_ACCEPT_ANDROID_LICENSE");
    if (before) {
        free(mark);
        return true;
    }
    printf("tide: Android builds need %s, which isn't on this machine.\n"
           "  tide can download it from Google (%s), under Google's license, the Android Software\n"
           "  Development Kit License Agreement: https://developer.android.com/studio/terms\n",
           what, size);
    if (!stdin_is_terminal()) {
        fprintf(stderr, "  = note: to accept it without being asked, set TIDE_ACCEPT_ANDROID_LICENSE=1; or install "
                        "Android Studio, whose SDK Manager has it\n");
        free(mark);
        return false;
    }
    printf("  Download it, and accept that license? [y/N] ");
    fflush(stdout);
    char line[16] = "";
    const bool yes = fgets(line, sizeof line, stdin) && (line[0] == 'y' || line[0] == 'Y');
    if (yes) {
        char *dir = path_dir(mark);
        sys_mkdirs(dir);
        free(dir);
        sys_write_text(mark, "Google's Android Software Development Kit License Agreement, accepted for tide's downloads\n");
    } else {
        fprintf(stderr, "tide: nothing was downloaded\n");
    }
    free(mark);
    return yes;
}

static bool sha1_matches(const char *path, const char *want)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    rtc_sha1 s;
    rtc_sha1_init(&s);
    static uint8_t chunk[1 << 16];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) rtc_sha1_add(&s, chunk, n);
    fclose(f);
    uint8_t hash[20];
    rtc_sha1_end(&s, hash);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    return strcmp(hex, want) == 0;
}

// Downloads one of Google's files into `work` and checks it; its path, or NULL.
static char *fetch(const char *work, const char *file, const char *sha1)
{
    sys_mkdirs(work);
    char *path = path_join(work, file);
    char url[256];
    snprintf(url, sizeof url, GOOGLE "%s", file);
    printf("Downloading %s...\n", file);
    fflush(stdout);
    const char *const argv[] = {sys_tool("curl"), "-fL", "--retry", "2", sys_is_terminal() ? "-#" : "-sS", "-o", path, url, NULL};
    if (sys_run(argv, NULL, false) != 0) {
        fprintf(stderr, "tide: downloading %s failed\n", url);
        free(path);
        return NULL;
    }
    if (!sha1_matches(path, sha1)) {
        fprintf(stderr, "tide: the download of %s is damaged (its checksum doesn't match)\n", file);
        sys_remove(path);
        free(path);
        return NULL;
    }
    return path;
}

// ---------------------------------------------------------------------------
// The NDK

// Its sysroot and compiler runtime, if `prebuilt` (its toolchain for this
// machine) has them for every CPU and Android 10.
static bool ndk_at(const char *prebuilt, android_ndk *ndk)
{
    char *sysroot = path_join(prebuilt, "sysroot");
    bool ok = true;
    for (int i = 0; ok && i < ANDROID_ABIS; i++) {
        char lib[256];
        snprintf(lib, sizeof lib, "usr/lib/%s-linux-android/" ANDROID_API "/libc.so", android_arches[i]);
        char *libc = path_join(sysroot, lib);
        ok = sys_exists(libc);
        free(libc);
    }
    if (!ok) {
        free(sysroot);
        return false;
    }
    // The runtime is in tide's own lib/, or under clang's version in Google's.
    char *clang = path_join(prebuilt, "lib/clang");
    for (int i = 0; ok && i < ANDROID_ABIS; i++) {
        char name[128];
        snprintf(name, sizeof name, "libclang_rt.builtins-%s-android.a", android_arches[i]);
        char *own = path_join(prebuilt, "lib");
        char *path = path_join(own, name);
        free(own);
        for (int v = 40; v >= 9 && !sys_exists(path); v--) {
            free(path);
            char at[256];
            snprintf(at, sizeof at, "%d/lib/linux/%s", v, name);
            path = path_join(clang, at);
        }
        if (!sys_exists(path)) {
            free(path);
            path = NULL;
        }
        ndk->builtins[i] = path;
        ok = path != NULL;
    }
    free(clang);
    if (ok) ndk->sysroot = sysroot;
    else free(sysroot);
    return ok;
}

// The newest NDK in an SDK's ndk/ folder that has what's needed.
typedef struct ndk_search {
    const char *folder;
    char best[64];
    android_ndk *ndk;
    bool found;
} ndk_search;

// Versions like 30.0.16248370, part by part.
static int version_compare(const char *a, const char *b)
{
    while (*a || *b) {
        char *a_end, *b_end;
        const long x = strtol(a, &a_end, 10), y = strtol(b, &b_end, 10);
        if (x != y) return x < y ? -1 : 1;
        a = *a_end == '.' ? a_end + 1 : a_end;
        b = *b_end == '.' ? b_end + 1 : b_end;
        if (a == a_end && b == b_end) return 0; // Past the numbers
    }
    return 0;
}

static void visit_ndk(void *user, const char *name, const bool is_dir)
{
    ndk_search *s = user;
    if (!is_dir || (name[0] < '0' || name[0] > '9')) return;
    if (s->found && version_compare(name, s->best) <= 0) return;
    char *dir = path_join(s->folder, name);
    char *prebuilt = path_join(dir, "toolchains/llvm/prebuilt/" HOST);
    android_ndk candidate = {0};
    if (ndk_at(prebuilt, &candidate)) {
        if (s->found) {
            free(s->ndk->sysroot);
            for (int i = 0; i < ANDROID_ABIS; i++) free(s->ndk->builtins[i]);
        }
        *s->ndk = candidate;
        s->found = true;
        snprintf(s->best, sizeof s->best, "%s", name);
    }
    free(prebuilt);
    free(dir);
}

// What tide keeps of Google's NDK: its sysroot's headers, the libraries for
// Android 10, the compiler runtime and its notices.
static bool ndk_member(void *user, const char *name, char *to, const size_t size)
{
    (void)user;
    const char *at = strstr(name, "/toolchains/llvm/prebuilt/" HOST "/");
    if (at) {
        const char *rest = at + strlen("/toolchains/llvm/prebuilt/" HOST "/");
        // C's headers, not C++'s: games are C (and C++'s go too deep for Windows' paths)
        if (strncmp(rest, "sysroot/usr/include/", 20) == 0 && strncmp(rest + 20, "c++/", 4) != 0) {
            snprintf(to, size, "%s", rest);
            return true;
        }
        for (int i = 0; i < ANDROID_ABIS; i++) {
            char lib[128];
            snprintf(lib, sizeof lib, "sysroot/usr/lib/%s-linux-android/" ANDROID_API "/", android_arches[i]);
            if (strncmp(rest, lib, strlen(lib)) == 0) {
                snprintf(to, size, "%s", rest);
                return true;
            }
            char builtins[128];
            snprintf(builtins, sizeof builtins, "/libclang_rt.builtins-%s-android.a", android_arches[i]);
            if (strncmp(rest, "lib/clang/", 10) == 0 && strstr(rest, "/lib/linux/") && strstr(rest, builtins)) {
                snprintf(to, size, "lib%s", builtins);
                return true;
            }
        }
        return false;
    }
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (strcmp(base, "NOTICE") == 0 && strchr(name, '/') == strrchr(name, '/')) { // The NDK's own, at its top
        snprintf(to, size, "NOTICE");
        return true;
    }
    return false;
}

bool android_find_ndk(const char *root, android_ndk *ndk)
{
    *ndk = (android_ndk){0};
    const char *vars[] = {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"};
    for (int i = 0; i < 2; i++) {
        if (!sys_env(vars[i])) continue;
        char *prebuilt = path_join(sys_env(vars[i]), "toolchains/llvm/prebuilt/" HOST);
        const bool ok = ndk_at(prebuilt, ndk);
        free(prebuilt);
        if (ok) return true;
        fprintf(stderr, "tide: %s doesn't hold an NDK that builds for Android 10 (r21 or newer)\n", vars[i]);
    }
    char *sdks[4];
    const int count = sdk_folders(sdks);
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (!found) {
            char *folder = path_join(sdks[i], "ndk");
            ndk_search s = {folder, "", ndk, false};
            sys_list(folder, visit_ndk, &s);
            found = s.found;
            free(folder);
        }
        free(sdks[i]);
    }
    if (found) return true;

    // tide's own, fetched once
    char *own = path_join(root, "android/ndk-r30");
    char *done = path_join(own, "done");
    if (sys_exists(done) && ndk_at(own, ndk)) {
        free(own);
        free(done);
        return true;
    }
    bool ok = agreed(root, "Google's Android NDK (its C library and headers)", "730 MB, of which tide keeps 20");
    if (ok) {
        char *work = path_join(root, "android/download");
        char *zip = fetch(work, NDK_ZIP, NDK_SHA1);
        ok = zip != NULL;
        if (ok) {
            printf("Unpacking what tide needs of it...\n");
            fflush(stdout);
            sys_remove_tree(own);
            ok = unzip(zip, own, ndk_member, NULL) && ndk_at(own, ndk);
            if (ok) sys_write_text(done, "r30\n");
            else fprintf(stderr, "tide: the NDK didn't have what tide needs\n");
        }
        sys_remove_tree(work);
        free(work);
        free(zip);
    }
    free(own);
    free(done);
    return ok;
}

// ---------------------------------------------------------------------------
// adb

static bool tools_member(void *user, const char *name, char *to, const size_t size)
{
    (void)user;
    if (strncmp(name, "platform-tools/", 15) != 0) return false;
    snprintf(to, size, "%s", name + 15);
    return true;
}

char *android_find_adb(const char *root)
{
    char *sdks[4];
    const int count = sdk_folders(sdks);
    char *found = NULL;
    for (int i = 0; i < count; i++) {
        char *adb = path_join(sdks[i], "platform-tools/adb" EXE);
        if (!found && sys_exists(adb)) found = adb;
        else free(adb);
        free(sdks[i]);
    }
    if (!found) found = sys_which("adb");
    if (found) return found;

    char *own = path_join(root, "android/platform-tools");
    char *adb = path_join(own, "adb" EXE);
    if (sys_exists(adb)) {
        free(own);
        return adb;
    }
    bool ok = agreed(root, "Google's platform tools (adb, which installs apps on phones)", "9 MB");
    if (ok) {
        char *work = path_join(root, "android/download");
        char *zip = fetch(work, TOOLS_ZIP, TOOLS_SHA1);
        ok = zip && unzip(zip, own, tools_member, NULL) && sys_exists(adb);
        sys_remove_tree(work);
        free(work);
        free(zip);
    }
    free(own);
    if (ok) return adb;
    free(adb);
    return NULL;
}

char *android_key_path(void)
{
    char *h = home();
    char *dir = path_join(h, ".android");
    sys_mkdirs(dir);
    char *key = path_join(dir, "tide.key");
    free(dir);
    free(h);
    return key;
}
