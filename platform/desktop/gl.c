// OpenGL's functions on desktop, looked up in the window's context (see
// src/gl.h): the system's library only names the oldest of them.

#include "gl.h"

#include <stdio.h>

tide_gl_functions tide_gl;

bool tide_gl_load(tide_gl_proc (*find)(const char *name))
{
    bool found = true;
#define LOAD(name)                                                                                                     \
    tide_gl.name = (__typeof__(tide_gl.name))find("gl" #name);                                                         \
    if (!tide_gl.name) {                                                                                               \
        fprintf(stderr, "tide: this machine's OpenGL has no gl" #name "\n");                                           \
        found = false;                                                                                                 \
    }
#define LOAD_V(name, parameters, arguments) LOAD(name)
#define LOAD_R(type, name, parameters, arguments) LOAD(name)
    TIDE_GL_FUNCTIONS(LOAD_V, LOAD_R)
#undef LOAD_V
#undef LOAD_R
#undef LOAD
    return found;
}
