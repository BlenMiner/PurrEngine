#include "version.h"

// Set by the build (compiler/CMakeLists.txt), the same for every tool made
// from one tree: tide, tidec, tidels and the host tidec of web builds.
#ifndef TIDE_VERSION
#define TIDE_VERSION "0.0.0-dev"
#endif

const char *tide_version_text(void)
{
    return TIDE_VERSION;
}

bool tide_version_parse(const char *text, const int len, int out[3])
{
    int part = 0;
    out[0] = out[1] = out[2] = 0;
    bool digits = false;
    for (int i = 0; i < len; i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            if (out[part] > 100000) return false;
            out[part] = out[part] * 10 + (c - '0');
            digits = true;
        } else if (c == '.' && digits && part < 2) {
            part++;
            digits = false;
        } else {
            return false;
        }
    }
    return digits && part >= 1;
}

bool tide_version_at_least(const int major, const int minor, const int patch)
{
    // The numbers before a pre-release's '-'.
    const char *text = TIDE_VERSION;
    int len = 0;
    while (text[len] && text[len] != '-') len++;
    int have[3];
    if (!tide_version_parse(text, len, have)) return false;
    const int want[3] = {major, minor, patch};
    for (int i = 0; i < 3; i++) {
        if (have[i] != want[i]) return have[i] > want[i];
    }
    return true;
}
