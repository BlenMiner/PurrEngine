#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// tide's built-in clang (cc.cpp). `argv` is tide's own: `tide cc <clang
// arguments>`, or `tide -cc1 ...` when clang runs itself again. Returns the
// exit code.
int tide_cc(int argc, const char **argv);

#ifdef __cplusplus
}
#endif
