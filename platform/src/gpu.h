#pragma once

// The GPU as the renderer (platform.c) uses it, whichever API is under it:
// buffers, textures, pipelines and passes. It's shaped by WebGPU, the
// strictest of them, so the others fit under it:
//
// - A pipeline is made once, with its shaders, the vertices it reads, and how
//   it blends and tests depth. Nothing about it changes afterwards.
// - A pipeline's uniforms are one block of bytes, set before a draw.
// - Drawing happens in a pass on a target, which starts cleared or as it was
//   left. There's no clear inside a pass: the renderer draws one.
//
// Each has one, which the window says (tide_window_gpu): OpenGL (gpu_gl.c, on
// gl.h) and, on the web, WebGPU (web/gpu_web.c, in tide.js). Vulkan is to come.
// Clip space is OpenGL's (depth from -1 to 1), pixels are counted from a
// target's top left, and textures' and targets' rows go from the top.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t tide_gpu_id; // A buffer, texture, pipeline or target; 0 is none, and the window as a target

typedef enum tide_gpu_buffer_kind { TIDE_GPU_VERTICES, TIDE_GPU_INDICES } tide_gpu_buffer_kind;

// A texture's pixels: 4 bytes each (red, green, blue, alpha), or 1 (red).
typedef enum tide_gpu_format { TIDE_GPU_RGBA8, TIDE_GPU_R8 } tide_gpu_format;

// What a vertex attribute is in its buffer: floats, or 4 bytes read as 0 to 1.
typedef enum tide_gpu_attribute_format {
    TIDE_GPU_FLOAT,
    TIDE_GPU_FLOAT2,
    TIDE_GPU_FLOAT3,
    TIDE_GPU_FLOAT4,
    TIDE_GPU_BYTES4,
} tide_gpu_attribute_format;

typedef enum tide_gpu_blend {
    TIDE_GPU_BLEND_ALPHA,   // Over what's there, by its alpha
    TIDE_GPU_BLEND_REPLACE, // In place of what's there, alpha too
} tide_gpu_blend;

typedef enum tide_gpu_depth {
    TIDE_GPU_DEPTH_NONE,  // Neither tested nor written
    TIDE_GPU_DEPTH_TEST,  // The nearest shows, and is written
    TIDE_GPU_DEPTH_RESET, // Written whatever is there: for clears
} tide_gpu_depth;

enum { TIDE_GPU_MAX_SLOTS = 2, TIDE_GPU_MAX_ATTRIBUTES = 8 };

// The most a pipeline's uniform block takes, in bytes.
#define TIDE_GPU_MAX_UNIFORMS 256u

typedef struct tide_gpu_pipeline_desc {
    const char *name; // For what the backend says when its shaders are wrong
    // Its shaders, of which a backend takes its own. OpenGL's: a vertex and a
    // fragment shader without their version line, their attributes at the
    // locations below, their uniforms in a std140 block named `uniforms`, and
    // their texture `pixels`. WebGPU's: both in one, `vertex` and `fragment`,
    // their uniforms at binding 0 of group 0, and their texture and its
    // sampler at bindings 1 and 2.
    const char *glsl_vertex, *glsl_fragment;
    const char *wgsl;
    // The vertex buffers it reads, by slot: how far apart their elements are,
    // and whether they're one for each vertex or for each instance.
    struct {
        uint32_t stride;
        bool per_instance;
    } slots[TIDE_GPU_MAX_SLOTS];
    uint32_t slot_count;
    // Its attributes, each at the location that's its place here.
    struct {
        uint32_t slot;
        tide_gpu_attribute_format format;
        uint32_t offset;
    } attributes[TIDE_GPU_MAX_ATTRIBUTES];
    uint32_t attribute_count;
    uint32_t uniform_size; // Its uniform block's, in bytes: every pipeline has one
    bool texture;          // Whether it samples one
    tide_gpu_blend blend;
    tide_gpu_depth depth;
} tide_gpu_pipeline_desc;

typedef struct tide_gpu {
    const char *name; // What draws: "WebGPU", "OpenGL 3.3"...

    // Which device there is to draw with: 0 while there's none, and another
    // number once one was lost and another found (WebGPU's can be: a driver
    // that starts over). Everything made on the last one went with it, and
    // nothing below is called while there's none.
    uint32_t (*epoch)(void);

    // A buffer of `size` bytes, holding `data` if it's given. Sizes, and what's
    // written, are multiples of 4 bytes.
    tide_gpu_id (*buffer)(tide_gpu_buffer_kind kind, size_t size, const void *data);
    void (*buffer_write)(tide_gpu_id buffer, size_t offset, const void *data, size_t size);
    void (*buffer_free)(tide_gpu_id buffer);

    // A texture, which doesn't repeat past its edges. Writing takes all of it.
    tide_gpu_id (*texture)(tide_gpu_format format, int width, int height, const void *pixels);
    void (*texture_write)(tide_gpu_id texture, const void *pixels);
    void (*texture_free)(tide_gpu_id texture);

    tide_gpu_id (*pipeline)(const tide_gpu_pipeline_desc *desc);

    // Pixels of its own to draw into, with depth, and to read back: 4 bytes
    // each, rows from the top.
    tide_gpu_id (*target)(int width, int height);
    void (*target_free)(tide_gpu_id target);
    void (*target_read)(tide_gpu_id target, uint8_t *rgba);

    // A pass on `target`, or the window for 0: cleared to black, depth too, or
    // over what's there. One at a time; everything below is inside one.
    void (*begin)(tide_gpu_id target, bool clear);
    void (*end)(void);
    // Only what's inside the rect draws from here on: the whole target, to
    // draw everywhere. It's inside the target.
    void (*scissor)(int x, int y, int width, int height);
    // The pipeline the draws after it use, then what it draws with.
    void (*use)(tide_gpu_id pipeline);
    void (*uniforms)(const void *data, size_t size);
    void (*sample)(tide_gpu_id texture, bool blended); // Between pixels: blended with its neighbors, or the nearest
    void (*vertices)(uint32_t slot, tide_gpu_id buffer, size_t offset);
    void (*indices)(tide_gpu_id buffer, bool wide); // 32 bits each, or 16
    // Triangles, three vertices (or indices) each, `instances` times over.
    void (*draw)(uint32_t first, uint32_t count, uint32_t instances);
    void (*draw_indexed)(uint32_t count, uint32_t instances);
} tide_gpu;

extern const tide_gpu tide_gpu_gl; // gpu_gl.c
#ifdef __wasm__
extern const tide_gpu tide_gpu_webgpu; // web/gpu_web.c
#endif
