#pragma once

// OpenGL as the platform layer uses it: the part OpenGL 3.3, OpenGL ES 3 and
// WebGL 2 share, and only what the renderer calls (platform.c). No system
// header: Windows' stops at OpenGL 1.1, and each platform keeps its own
// elsewhere.
//
// On the web the functions are the page's (platform/web/tide.js gives each by
// its C name), and on Android libGLESv3's. On desktop they're looked up in the
// window's context once it's current (tide_gl_load, gl.c), and the names
// below call through what it found.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

enum {
    GL_FALSE = 0,
    GL_TRIANGLES = 0x0004,
    GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_COLOR_BUFFER_BIT = 0x4000,
    GL_LEQUAL = 0x0203,
    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71,
    GL_BLEND = 0x0BE2,
    GL_SCISSOR_TEST = 0x0C11,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05,
    GL_TEXTURE_2D = 0x0DE1,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_RED = 0x1903,
    GL_RGBA = 0x1908,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_RGBA8 = 0x8058,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_DEPTH_COMPONENT24 = 0x81A6,
    GL_R8 = 0x8229,
    GL_TEXTURE0 = 0x84C0,
    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_STATIC_DRAW = 0x88E4,
    GL_DYNAMIC_DRAW = 0x88E8,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_VERTEX_SHADER = 0x8B31,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_FRAMEBUFFER = 0x8D40,
    GL_RENDERBUFFER = 0x8D41,
};

// Every function: V for those that return nothing, R for the others, each
// with its name after `gl`, its parameters, and those as arguments.
#define TIDE_GL_FUNCTIONS(V, R)                                                                                        \
    V(ActiveTexture, (GLenum texture), (texture))                                                                      \
    V(AttachShader, (GLuint program, GLuint shader), (program, shader))                                                \
    V(BindBuffer, (GLenum target, GLuint buffer), (target, buffer))                                                    \
    V(BindFramebuffer, (GLenum target, GLuint framebuffer), (target, framebuffer))                                     \
    V(BindRenderbuffer, (GLenum target, GLuint renderbuffer), (target, renderbuffer))                                  \
    V(BindTexture, (GLenum target, GLuint texture), (target, texture))                                                 \
    V(BindVertexArray, (GLuint array), (array))                                                                        \
    V(BlendFunc, (GLenum source, GLenum destination), (source, destination))                                           \
    V(BufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage), (target, size, data, usage))       \
    V(BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data), (target, offset, size, data)) \
    R(GLenum, CheckFramebufferStatus, (GLenum target), (target))                                                       \
    V(Clear, (GLbitfield mask), (mask))                                                                                \
    V(ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a), (r, g, b, a))                                          \
    V(CompileShader, (GLuint shader), (shader))                                                                        \
    R(GLuint, CreateProgram, (void), ())                                                                               \
    R(GLuint, CreateShader, (GLenum type), (type))                                                                     \
    V(DeleteBuffers, (GLsizei count, const GLuint *buffers), (count, buffers))                                         \
    V(DeleteFramebuffers, (GLsizei count, const GLuint *framebuffers), (count, framebuffers))                          \
    V(DeleteRenderbuffers, (GLsizei count, const GLuint *renderbuffers), (count, renderbuffers))                       \
    V(DeleteShader, (GLuint shader), (shader))                                                                         \
    V(DeleteTextures, (GLsizei count, const GLuint *textures), (count, textures))                                      \
    V(DepthFunc, (GLenum function), (function))                                                                        \
    V(Disable, (GLenum capability), (capability))                                                                      \
    V(DrawArrays, (GLenum mode, GLint first, GLsizei count), (mode, first, count))                                     \
    V(DrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instances),                               \
      (mode, first, count, instances))                                                                                 \
    V(DrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances),        \
      (mode, count, type, indices, instances))                                                                         \
    V(Enable, (GLenum capability), (capability))                                                                       \
    V(EnableVertexAttribArray, (GLuint index), (index))                                                                \
    V(FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum renderbuffer_target, GLuint renderbuffer),    \
      (target, attachment, renderbuffer_target, renderbuffer))                                                         \
    V(GenBuffers, (GLsizei count, GLuint *buffers), (count, buffers))                                                  \
    V(GenFramebuffers, (GLsizei count, GLuint *framebuffers), (count, framebuffers))                                   \
    V(GenRenderbuffers, (GLsizei count, GLuint *renderbuffers), (count, renderbuffers))                                \
    V(GenTextures, (GLsizei count, GLuint *textures), (count, textures))                                               \
    V(GenVertexArrays, (GLsizei count, GLuint *arrays), (count, arrays))                                               \
    V(GetProgramInfoLog, (GLuint program, GLsizei capacity, GLsizei *length, GLchar *log),                             \
      (program, capacity, length, log))                                                                                \
    V(GetProgramiv, (GLuint program, GLenum name, GLint *out), (program, name, out))                                   \
    V(GetShaderInfoLog, (GLuint shader, GLsizei capacity, GLsizei *length, GLchar *log),                               \
      (shader, capacity, length, log))                                                                                 \
    V(GetShaderiv, (GLuint shader, GLenum name, GLint *out), (shader, name, out))                                      \
    R(GLint, GetUniformLocation, (GLuint program, const GLchar *name), (program, name))                                \
    V(LinkProgram, (GLuint program), (program))                                                                        \
    V(PixelStorei, (GLenum name, GLint value), (name, value))                                                          \
    V(ReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels),         \
      (x, y, width, height, format, type, pixels))                                                                     \
    V(RenderbufferStorage, (GLenum target, GLenum format, GLsizei width, GLsizei height),                              \
      (target, format, width, height))                                                                                 \
    V(Scissor, (GLint x, GLint y, GLsizei width, GLsizei height), (x, y, width, height))                               \
    V(ShaderSource, (GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths),                \
      (shader, count, strings, lengths))                                                                               \
    V(TexImage2D,                                                                                                      \
      (GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border, GLenum format,         \
       GLenum type, const void *pixels),                                                                               \
      (target, level, internal, width, height, border, format, type, pixels))                                          \
    V(TexParameteri, (GLenum target, GLenum name, GLint value), (target, name, value))                                 \
    V(TexSubImage2D,                                                                                                   \
      (GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,        \
       const void *pixels),                                                                                            \
      (target, level, x, y, width, height, format, type, pixels))                                                      \
    V(Uniform1i, (GLint location, GLint value), (location, value))                                                     \
    V(Uniform2f, (GLint location, GLfloat x, GLfloat y), (location, x, y))                                             \
    V(UniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *values),                   \
      (location, count, transpose, values))                                                                            \
    V(UseProgram, (GLuint program), (program))                                                                         \
    V(VertexAttribDivisor, (GLuint index, GLuint divisor), (index, divisor))                                           \
    V(VertexAttribPointer,                                                                                             \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer),              \
      (index, size, type, normalized, stride, pointer))                                                                \
    V(Viewport, (GLint x, GLint y, GLsizei width, GLsizei height), (x, y, width, height))

#if defined(__wasm__) || defined(__ANDROID__)

#define TIDE_GL_DECLARE_V(name, parameters, arguments) void gl##name parameters;
#define TIDE_GL_DECLARE_R(type, name, parameters, arguments) type gl##name parameters;
TIDE_GL_FUNCTIONS(TIDE_GL_DECLARE_V, TIDE_GL_DECLARE_R)
#undef TIDE_GL_DECLARE_V
#undef TIDE_GL_DECLARE_R

#else

// Windows' OpenGL functions are stdcall, which only 32-bit x86 tells from
// the default.
#ifdef _WIN32
#define TIDE_GL_CALL __stdcall
#else
#define TIDE_GL_CALL
#endif

#define TIDE_GL_MEMBER_V(name, parameters, arguments) void(TIDE_GL_CALL *name) parameters;
#define TIDE_GL_MEMBER_R(type, name, parameters, arguments) type(TIDE_GL_CALL *name) parameters;
typedef struct tide_gl_functions {
    TIDE_GL_FUNCTIONS(TIDE_GL_MEMBER_V, TIDE_GL_MEMBER_R)
} tide_gl_functions;
#undef TIDE_GL_MEMBER_V
#undef TIDE_GL_MEMBER_R

extern tide_gl_functions tide_gl;

// Looks every function up with `find` (glfwGetProcAddress), in the context
// that's current. False, having said which, if the context lacks one.
typedef void (*tide_gl_proc)(void);
bool tide_gl_load(tide_gl_proc (*find)(const char *name));

#define TIDE_GL_CALL_V(name, parameters, arguments)                                                                    \
    static inline void gl##name parameters                                                                             \
    {                                                                                                                  \
        tide_gl.name arguments;                                                                                        \
    }
#define TIDE_GL_CALL_R(type, name, parameters, arguments)                                                              \
    static inline type gl##name parameters                                                                             \
    {                                                                                                                  \
        return tide_gl.name arguments;                                                                                 \
    }
TIDE_GL_FUNCTIONS(TIDE_GL_CALL_V, TIDE_GL_CALL_R)
#undef TIDE_GL_CALL_V
#undef TIDE_GL_CALL_R

#endif
