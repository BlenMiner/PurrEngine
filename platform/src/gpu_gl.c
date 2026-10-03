// OpenGL under the renderer (see gpu.h): OpenGL 3.3 on desktop, OpenGL ES 3 on
// Android and WebGL 2 on the web, through gl.h. Its buffers' and textures'
// ids are OpenGL's own names for them.

#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/page.h"
#include "gl.h"
#include "window.h"

// One GLSL for all three, but for its first lines.
#if defined(__wasm__)
#define GL_NAME "WebGL 2"
#define GLSL_VERSION "#version 300 es\nprecision highp float;\n"
#elif defined(__ANDROID__)
#define GL_NAME "OpenGL ES 3"
#define GLSL_VERSION "#version 300 es\nprecision highp float;\n"
#else
#define GL_NAME "OpenGL 3.3"
#define GLSL_VERSION "#version 330\n"
#endif

typedef struct gl_pipeline {
    GLuint program, vao;
    tide_gpu_pipeline_desc desc; // Its slots and attributes, and how it blends and tests depth
} gl_pipeline;

typedef struct gl_target {
    GLuint framebuffer, color, depth; // No framebuffer: a place that's free
    int width, height;
} gl_target;

// What's known of a buffer or a texture, by its name.
typedef struct gl_buffer {
    uint8_t kind; // tide_gpu_buffer_kind: WebGL binds a buffer as one kind only, ever
} gl_buffer;

typedef struct gl_texture {
    uint8_t format;  // tide_gpu_format
    int8_t blended;  // How it's sampled now
    int32_t width, height;
} gl_texture;

static gl_pipeline *pipelines; // A pipeline's id is 1 + its place, and a target's
static uint32_t pipeline_count;
static gl_target *targets;
static uint32_t target_count;
static gl_buffer *buffers; // By name
static uint32_t buffer_names;
static gl_texture *textures;
static uint32_t texture_names;

static GLuint upload_vao;     // What index buffers are bound in while they're written, in no pipeline's place
static GLuint uniform_buffer; // Every pipeline's uniform block, as the last draw's was set
static const gl_pipeline *current;
static GLenum index_type;
static int pass_height; // The pass's target's, in its pixels

// Room for names up to `name` in an array of `size`-byte elements, zeroed.
static void *room(void *array, uint32_t *count, const uint32_t name, const size_t size)
{
    if (name < *count) return array;
    uint32_t grown = *count ? *count : 64u;
    while (grown <= name) grown *= 2u;
    array = tide_realloc(array, *count * size, grown * size);
    memset((char *)array + *count * size, 0, (grown - *count) * size);
    *count = grown;
    return array;
}

// Binds a buffer where its kind goes, and says where that is.
static GLenum bind_buffer(const GLuint buffer)
{
    if (buffers[buffer].kind == TIDE_GPU_VERTICES) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        return GL_ARRAY_BUFFER;
    }
    // A vertex array keeps its index buffer: not a pipeline's
    if (!upload_vao) glGenVertexArrays(1, &upload_vao);
    glBindVertexArray(upload_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    return GL_ELEMENT_ARRAY_BUFFER;
}

static tide_gpu_id gl_buffer_make(const tide_gpu_buffer_kind kind, const size_t size, const void *data)
{
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    buffers = room(buffers, &buffer_names, buffer, sizeof(gl_buffer));
    buffers[buffer].kind = (uint8_t)kind;
    glBufferData(bind_buffer(buffer), (GLsizeiptr)size, data, data ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW);
    return buffer;
}

static void gl_buffer_write(const tide_gpu_id buffer, const size_t offset, const void *data, const size_t size)
{
    glBufferSubData(bind_buffer(buffer), (GLintptr)offset, (GLsizeiptr)size, data);
}

static void gl_buffer_free(const tide_gpu_id buffer)
{
    glDeleteBuffers(1, &buffer);
}

static void texture_pixels(const gl_texture *t, const void *pixels, const bool again)
{
    const bool red = t->format == TIDE_GPU_R8;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); // Rows of any width, one after the other
    if (again) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->width, t->height, red ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, red ? GL_R8 : GL_RGBA8, t->width, t->height, 0, red ? GL_RED : GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
    }
}

static tide_gpu_id gl_texture_make(const tide_gpu_format format, const int width, const int height, const void *pixels)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    textures = room(textures, &texture_names, texture, sizeof(gl_texture));
    textures[texture] = (gl_texture){.format = (uint8_t)format, .width = width, .height = height};
    glBindTexture(GL_TEXTURE_2D, texture);
    texture_pixels(&textures[texture], pixels, false);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

static void gl_texture_write(const tide_gpu_id texture, const void *pixels)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    texture_pixels(&textures[texture], pixels, true);
}

static void gl_texture_free(const tide_gpu_id texture)
{
    glDeleteTextures(1, &texture);
}

static GLuint compile(const char *name, const GLenum kind, const char *source)
{
    const GLuint shader = glCreateShader(kind);
    const char *const sources[2] = {GLSL_VERSION, source};
    glShaderSource(shader, 2, sources, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "tide: the %s' %s shader didn't compile:\n%s\n", name,
                kind == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        abort();
    }
    return shader;
}

static tide_gpu_id gl_pipeline_make(const tide_gpu_pipeline_desc *desc)
{
    const GLuint vertex = compile(desc->name, GL_VERTEX_SHADER, desc->glsl_vertex);
    const GLuint fragment = compile(desc->name, GL_FRAGMENT_SHADER, desc->glsl_fragment);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        fprintf(stderr, "tide: the %s' shaders didn't link:\n%s\n", desc->name, log);
        abort();
    }
    glDeleteShader(vertex); // The program keeps them
    glDeleteShader(fragment);
    // Its uniforms are in the uniform buffer, and its texture the first
    const GLuint block = glGetUniformBlockIndex(program, "uniforms");
    if (block != GL_INVALID_INDEX) glUniformBlockBinding(program, block, 0);
    if (desc->texture) {
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "pixels"), 0);
    }

    pipelines = tide_realloc(pipelines, pipeline_count * sizeof(gl_pipeline), (pipeline_count + 1u) * sizeof(gl_pipeline));
    gl_pipeline *p = &pipelines[pipeline_count++];
    *p = (gl_pipeline){.program = program, .desc = *desc};
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    for (uint32_t i = 0; i < desc->attribute_count; i++) {
        glEnableVertexAttribArray(i);
        if (desc->slots[desc->attributes[i].slot].per_instance) glVertexAttribDivisor(i, 1);
    }
    current = NULL; // The pipelines may have moved
    return pipeline_count;
}

static tide_gpu_id gl_target_make(const int width, const int height)
{
    uint32_t place = 0;
    while (place < target_count && targets[place].framebuffer) place++;
    if (place == target_count) {
        targets = tide_realloc(targets, target_count * sizeof(gl_target), (target_count + 1u) * sizeof(gl_target));
        target_count++;
    }
    gl_target *t = &targets[place];
    *t = (gl_target){.width = width, .height = height};
    glGenFramebuffers(1, &t->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
    glGenRenderbuffers(1, &t->color);
    glBindRenderbuffer(GL_RENDERBUFFER, t->color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->color);
    glGenRenderbuffers(1, &t->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "tide: no framebuffer to render offscreen in\n");
        abort();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return place + 1u;
}

static void gl_target_free(const tide_gpu_id target)
{
    gl_target *t = &targets[target - 1u];
    glDeleteFramebuffers(1, &t->framebuffer);
    glDeleteRenderbuffers(1, &t->color);
    glDeleteRenderbuffers(1, &t->depth);
    *t = (gl_target){0};
}

static void gl_target_read(const tide_gpu_id target, uint8_t *rgba)
{
    const gl_target *t = &targets[target - 1u];
    glBindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, t->width, t->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // OpenGL's rows go from the bottom
    const size_t row = (size_t)t->width * 4u;
    uint8_t swap[1024];
    for (int y = 0; y < t->height / 2; y++) {
        uint8_t *top = rgba + (size_t)y * row, *bottom = rgba + (size_t)(t->height - 1 - y) * row;
        for (size_t at = 0; at < row; at += sizeof swap) {
            const size_t n = row - at < sizeof swap ? row - at : sizeof swap;
            memcpy(swap, top + at, n);
            memcpy(top + at, bottom + at, n);
            memcpy(bottom + at, swap, n);
        }
    }
}

static void gl_begin(const tide_gpu_id target, const bool clear)
{
    int width, height;
    if (target) {
        const gl_target *t = &targets[target - 1u];
        glBindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
        width = t->width, height = t->height;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        tide_window_pixel_size(&width, &height);
    }
    pass_height = height;
    // What a pass draws with is set here, whatever drew before: another
    // target's pass or, on the web, the program this one took over from.
    glViewport(0, 0, width, height);
    glDisable(GL_CULL_FACE); // Either side of a triangle draws
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);
    if (!uniform_buffer) {
        glGenBuffers(1, &uniform_buffer);
        glBindBuffer(GL_UNIFORM_BUFFER, uniform_buffer);
        glBufferData(GL_UNIFORM_BUFFER, TIDE_GPU_MAX_UNIFORMS, NULL, GL_DYNAMIC_DRAW);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform_buffer);
    current = NULL;
    if (clear) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
}

static void gl_end(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void gl_scissor(const int x, const int y, const int width, const int height)
{
    glScissor(x, pass_height - (y + height), width, height); // OpenGL's are from the bottom left
}

static void gl_use(const tide_gpu_id pipeline)
{
    current = &pipelines[pipeline - 1u];
    glUseProgram(current->program);
    glBindVertexArray(current->vao);
    if (current->desc.blend == TIDE_GPU_BLEND_ALPHA) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    // OpenGL only writes depth where it tests it
    if (current->desc.depth == TIDE_GPU_DEPTH_NONE) {
        glDisable(GL_DEPTH_TEST);
    } else {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(current->desc.depth == TIDE_GPU_DEPTH_TEST ? GL_LEQUAL : GL_ALWAYS);
    }
}

static void gl_uniforms(const void *data, const size_t size)
{
    glBindBuffer(GL_UNIFORM_BUFFER, uniform_buffer);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)size, data);
}

static void gl_sample(const tide_gpu_id texture, const bool blended)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    gl_texture *t = &textures[texture];
    if (t->blended != (int8_t)blended) {
        const GLint filter = blended ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        t->blended = (int8_t)blended;
    }
}

static void gl_vertices(const uint32_t slot, const tide_gpu_id buffer, const size_t offset)
{
    static const GLint counts[] = {1, 2, 3, 4, 4};
    const tide_gpu_pipeline_desc *d = &current->desc;
    glBindVertexArray(current->vao); // Writing an index buffer binds another
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    for (uint32_t i = 0; i < d->attribute_count; i++) {
        if (d->attributes[i].slot != slot) continue;
        const bool bytes = d->attributes[i].format == TIDE_GPU_BYTES4;
        glVertexAttribPointer(i, counts[d->attributes[i].format], bytes ? GL_UNSIGNED_BYTE : GL_FLOAT, bytes,
                              (GLsizei)d->slots[slot].stride, (const void *)(offset + d->attributes[i].offset));
    }
}

static void gl_indices(const tide_gpu_id buffer, const bool wide)
{
    glBindVertexArray(current->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    index_type = wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
}

static void gl_draw(const uint32_t first, const uint32_t count, const uint32_t instances)
{
    glBindVertexArray(current->vao);
    glDrawArraysInstanced(GL_TRIANGLES, (GLint)first, (GLsizei)count, (GLsizei)instances);
}

static void gl_draw_indexed(const uint32_t count, const uint32_t instances)
{
    glBindVertexArray(current->vao);
    glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)count, index_type, NULL, (GLsizei)instances);
}

// The window's context, which stays.
static uint32_t gl_epoch(void)
{
    return 1;
}

const tide_gpu tide_gpu_gl = {
    .name = GL_NAME,
    .epoch = gl_epoch,
    .buffer = gl_buffer_make,
    .buffer_write = gl_buffer_write,
    .buffer_free = gl_buffer_free,
    .texture = gl_texture_make,
    .texture_write = gl_texture_write,
    .texture_free = gl_texture_free,
    .pipeline = gl_pipeline_make,
    .target = gl_target_make,
    .target_free = gl_target_free,
    .target_read = gl_target_read,
    .begin = gl_begin,
    .end = gl_end,
    .scissor = gl_scissor,
    .use = gl_use,
    .uniforms = gl_uniforms,
    .sample = gl_sample,
    .vertices = gl_vertices,
    .indices = gl_indices,
    .draw = gl_draw,
    .draw_indexed = gl_draw_indexed,
};
