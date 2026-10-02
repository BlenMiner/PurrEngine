// tide_apk: makes an Android app (apk.c), for Android builds of this repo
// (tide_android_app in platform/CMakeLists.txt). tide makes them itself.
//
//   tide_apk <app.apk> --package <id> --label <name> --lib <name> --key <file>
//            [--min <api>] [--target <api>] [--debuggable] [--icon <png>] <abi>=<lib.so>...
//   tide_apk --key <file>   makes the key, unless it's there

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apk.h"

static int usage(void)
{
    fprintf(stderr, "usage: tide_apk <app.apk> --package <id> --label <name> --lib <name> --key <file>\n"
                    "                [--min <api>] [--target <api>] [--debuggable] <abi>=<lib.so>...\n"
                    "       tide_apk --key <file>\n");
    return 2;
}

int main(int argc, char **argv)
{
    char error[512];
    apk_key key;
    if (argc == 3 && strcmp(argv[1], "--key") == 0) {
        if (apk_key_load(argv[2], &key, error, sizeof error)) return 0;
        fprintf(stderr, "tide_apk: %s\n", error);
        return 1;
    }
    if (argc < 2) return usage();
    apk_desc desc = {.version_code = 1, .version_name = "1.0", .min_sdk = 29, .target_sdk = 35};
    const char *key_path = NULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const bool has_value = i + 1 < argc;
        if (strcmp(a, "--package") == 0 && has_value) desc.package = argv[++i];
        else if (strcmp(a, "--label") == 0 && has_value) desc.label = argv[++i];
        else if (strcmp(a, "--lib") == 0 && has_value) desc.lib_name = argv[++i];
        else if (strcmp(a, "--key") == 0 && has_value) key_path = argv[++i];
        else if (strcmp(a, "--min") == 0 && has_value) desc.min_sdk = atoi(argv[++i]);
        else if (strcmp(a, "--target") == 0 && has_value) desc.target_sdk = atoi(argv[++i]);
        else if (strcmp(a, "--debuggable") == 0) desc.debuggable = true;
        else if (strcmp(a, "--icon") == 0 && has_value) desc.icon = argv[++i];
        else if (strchr(a, '=') && a[0] != '-' && desc.lib_count < APK_MAX_LIBS) {
            static char abis[APK_MAX_LIBS][32];
            const size_t n = (size_t)(strchr(a, '=') - a);
            if (n >= sizeof abis[0]) return usage();
            memcpy(abis[desc.lib_count], a, n);
            abis[desc.lib_count][n] = '\0';
            desc.abis[desc.lib_count] = abis[desc.lib_count];
            desc.libs[desc.lib_count++] = a + n + 1;
        } else {
            return usage();
        }
    }
    if (!desc.package || !desc.label || !desc.lib_name || !key_path || desc.lib_count == 0) return usage();

    if (!apk_key_load(key_path, &key, error, sizeof error) || !apk_write(argv[1], &desc, &key, error, sizeof error)) {
        fprintf(stderr, "tide_apk: %s\n", error);
        return 1;
    }
    return 0;
}
