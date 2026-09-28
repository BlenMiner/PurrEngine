#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// purr's built-in clang (cc.cpp). `argv` is purr's own: `purr cc <clang
// arguments>`, or `purr -cc1 ...` when clang runs itself again. Returns the
// exit code.
int purr_cc(int argc, const char **argv);

#ifdef __cplusplus
}
#endif
