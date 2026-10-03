#pragma once

// The renderer's shaders (platform.c), each written for every backend (see
// gpu.h): GLSL for OpenGL, without its version line, and on the web WGSL for
// WebGPU. They draw the same pixels. Until Tide code compiles to them, they're
// written by hand, so a change to one goes to the other.

// WGSL is only the web's.
#ifdef __wasm__
#define WGSL(source) source
#else
#define WGSL(source) NULL
#endif

// Shapes: each instance's quad covers its shape in window pixels, which the
// camera already applied, and `screen` takes to the target.

static const char shape_glsl_vertex[] =
    "layout(location = 0) in vec2 corner;\n" // A corner of the quad, 0 to 1
    "layout(location = 1) in vec4 shape;\n"  // a, then b
    "layout(location = 2) in vec4 color;\n"
    "layout(location = 3) in float kind;\n"
    "layout(std140) uniform uniforms { mat4 screen; };\n"
    "out vec2 local;\n"  // Pixels from its middle; for a line, along and across it
    "out vec2 extent;\n" // Half its size in pixels
    "out vec4 tint;\n"
    "out float form;\n"
    "void main() {\n"
    "    vec2 c = corner * 2.0 - 1.0;\n"
    "    vec2 p;\n"
    "    if (kind > 3.5) {\n" // A line: a pixel wide, reaching half a pixel past its ends
    "        vec2 d = shape.zw - shape.xy;\n"
    "        float len = length(d);\n"
    "        vec2 along = len > 0.0 ? d / len : vec2(1.0, 0.0);\n"
    "        extent = vec2(len * 0.5 + 0.5, 0.5);\n"
    "        local = c * extent;\n"
    "        p = (shape.xy + shape.zw) * 0.5 + along * local.x + vec2(-along.y, along.x) * local.y;\n"
    "    } else {\n"
    "        extent = abs(shape.zw);\n"
    "        local = c * (extent + (kind > 1.5 ? 1.0 : 0.0));\n" // Room for a circle's smooth edge
    "        p = shape.xy + local;\n"
    "    }\n"
    "    tint = color;\n"
    "    form = kind;\n"
    "    gl_Position = screen * vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char shape_glsl_fragment[] =
    "in vec2 local;\n"
    "in vec2 extent;\n"
    "in vec4 tint;\n"
    "in float form;\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    float cover = 1.0;\n"
    "    if (form > 0.5 && form < 1.5) {\n" // A rect's outline: the pixel inside its edge
    "        if (all(lessThan(abs(local), extent - 1.0))) discard;\n"
    "    } else if (form > 1.5 && form < 3.5) {\n" // Circles, with a smooth edge
    "        float d = length(local);\n"
    "        cover = clamp(extent.x - d + 0.5, 0.0, 1.0);\n"
    "        if (form > 2.5) cover *= clamp(d - extent.x + 1.5, 0.0, 1.0);\n" // An outline: the pixel inside it
    "        if (cover <= 0.0) discard;\n"
    "    }\n"
    "    pixel = vec4(tint.rgb, tint.a * cover);\n"
    "}\n";

static const char *const shape_wgsl = WGSL(
    "struct Uniforms { screen: mat4x4f }\n"
    "@group(0) @binding(0) var<uniform> u: Uniforms;\n"
    "struct Corner {\n"
    "    @builtin(position) position: vec4f,\n"
    "    @location(0) local: vec2f,\n"
    "    @location(1) extent: vec2f,\n"
    "    @location(2) tint: vec4f,\n"
    "    @location(3) form: f32,\n"
    "}\n"
    "@vertex fn vertex(@location(0) corner: vec2f, @location(1) shape: vec4f, @location(2) color: vec4f,\n"
    "                  @location(3) kind: f32) -> Corner {\n"
    "    let c = corner * 2.0 - 1.0;\n"
    "    var out: Corner;\n"
    "    var p: vec2f;\n"
    "    if (kind > 3.5) {\n"
    "        let d = shape.zw - shape.xy;\n"
    "        let len = length(d);\n"
    "        var along = vec2f(1.0, 0.0);\n"
    "        if (len > 0.0) { along = d / len; }\n"
    "        out.extent = vec2f(len * 0.5 + 0.5, 0.5);\n"
    "        out.local = c * out.extent;\n"
    "        p = (shape.xy + shape.zw) * 0.5 + along * out.local.x + vec2f(-along.y, along.x) * out.local.y;\n"
    "    } else {\n"
    "        out.extent = abs(shape.zw);\n"
    "        var room = 0.0;\n"
    "        if (kind > 1.5) { room = 1.0; }\n"
    "        out.local = c * (out.extent + room);\n"
    "        p = shape.xy + out.local;\n"
    "    }\n"
    "    out.tint = color;\n"
    "    out.form = kind;\n"
    "    out.position = u.screen * vec4f(p, 0.0, 1.0);\n"
    "    return out;\n"
    "}\n"
    "@fragment fn fragment(in: Corner) -> @location(0) vec4f {\n"
    "    var cover = 1.0;\n"
    "    if (in.form > 0.5 && in.form < 1.5) {\n"
    "        if (all(abs(in.local) < in.extent - 1.0)) { discard; }\n"
    "    } else if (in.form > 1.5 && in.form < 3.5) {\n"
    "        let d = length(in.local);\n"
    "        cover = clamp(in.extent.x - d + 0.5, 0.0, 1.0);\n"
    "        if (in.form > 2.5) { cover *= clamp(d - in.extent.x + 1.5, 0.0, 1.0); }\n"
    "        if (cover <= 0.0) { discard; }\n"
    "    }\n"
    "    return vec4f(in.tint.rgb, in.tint.a * cover);\n"
    "}\n");

// Meshes: a mesh's pixels are its corners' colors, blended across each
// triangle, times its texture's (a white pixel, for a mesh with none).

static const char mesh_glsl_vertex[] =
    "layout(location = 0) in vec2 position;\n" // In window pixels
    "layout(location = 1) in vec2 uv;\n"
    "layout(location = 2) in vec4 color;\n"
    "layout(std140) uniform uniforms { mat4 screen; vec2 texel; };\n"
    "out vec2 at;\n"
    "out vec4 tint;\n"
    "void main() {\n"
    "    at = uv;\n"
    "    tint = color;\n"
    "    gl_Position = screen * vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char mesh_glsl_fragment[] =
    "in vec2 at;\n"
    "in vec4 tint;\n"
    "uniform sampler2D pixels;\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    pixel = texture(pixels, at) * tint;\n"
    "}\n";

#define MESH_WGSL_VERTEX                                                                                               \
    "struct Uniforms { screen: mat4x4f, texel: vec2f }\n"                                                              \
    "@group(0) @binding(0) var<uniform> u: Uniforms;\n"                                                                \
    "@group(0) @binding(1) var pixels: texture_2d<f32>;\n"                                                             \
    "@group(0) @binding(2) var how: sampler;\n"                                                                        \
    "struct Corner {\n"                                                                                                \
    "    @builtin(position) position: vec4f,\n"                                                                        \
    "    @location(0) at: vec2f,\n"                                                                                    \
    "    @location(1) tint: vec4f,\n"                                                                                  \
    "}\n"                                                                                                              \
    "@vertex fn vertex(@location(0) position: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f) -> Corner {\n" \
    "    var out: Corner;\n"                                                                                           \
    "    out.at = uv;\n"                                                                                               \
    "    out.tint = color;\n"                                                                                          \
    "    out.position = u.screen * vec4f(position, 0.0, 1.0);\n"                                                       \
    "    return out;\n"                                                                                                \
    "}\n"

static const char *const mesh_wgsl = WGSL(MESH_WGSL_VERTEX
    "@fragment fn fragment(in: Corner) -> @location(0) vec4f {\n"
    "    return textureSample(pixels, how, in.at) * in.tint;\n"
    "}\n");

// Text: its pixels are its color, as much as the font atlas says its glyph
// covers each. Its corners are a mesh's, their uv in the atlas's pixels, so
// the atlas can grow under them: `texel` is 1 over its size.

static const char text_glsl_fragment[] =
    "in vec2 at;\n"
    "in vec4 tint;\n"
    "uniform sampler2D pixels;\n"
    "layout(std140) uniform uniforms { mat4 screen; vec2 texel; };\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    pixel = vec4(tint.rgb, tint.a * texture(pixels, at * texel).r);\n"
    "}\n";

static const char *const text_wgsl = WGSL(MESH_WGSL_VERTEX
    "@fragment fn fragment(in: Corner) -> @location(0) vec4f {\n"
    "    return vec4f(in.tint.rgb, in.tint.a * textureSample(pixels, how, in.at * u.texel).r);\n"
    "}\n");

// 3D meshes: a mesh's corners go through its instance's transform and the
// camera, with depth; its pixels are as a mesh's.

static const char mesh_3d_glsl_vertex[] =
    "layout(location = 0) in vec3 position;\n"
    "layout(location = 1) in vec2 uv;\n"
    "layout(location = 2) in vec4 color;\n"
    "layout(location = 3) in vec4 model0;\n" // Its instance's transform, column by column
    "layout(location = 4) in vec4 model1;\n"
    "layout(location = 5) in vec4 model2;\n"
    "layout(location = 6) in vec4 model3;\n"
    "layout(std140) uniform uniforms { mat4 camera; };\n" // From the world to clip space
    "out vec2 at;\n"
    "out vec4 tint;\n"
    "void main() {\n"
    "    at = uv;\n"
    "    tint = color;\n"
    "    gl_Position = camera * (mat4(model0, model1, model2, model3) * vec4(position, 1.0));\n"
    "}\n";

static const char *const mesh_3d_wgsl = WGSL(
    "struct Uniforms { camera: mat4x4f }\n"
    "@group(0) @binding(0) var<uniform> u: Uniforms;\n"
    "@group(0) @binding(1) var pixels: texture_2d<f32>;\n"
    "@group(0) @binding(2) var how: sampler;\n"
    "struct Corner {\n"
    "    @builtin(position) position: vec4f,\n"
    "    @location(0) at: vec2f,\n"
    "    @location(1) tint: vec4f,\n"
    "}\n"
    "@vertex fn vertex(@location(0) position: vec3f, @location(1) uv: vec2f, @location(2) color: vec4f,\n"
    "                  @location(3) model0: vec4f, @location(4) model1: vec4f, @location(5) model2: vec4f,\n"
    "                  @location(6) model3: vec4f) -> Corner {\n"
    "    let p = u.camera * (mat4x4f(model0, model1, model2, model3) * vec4f(position, 1.0));\n"
    "    var out: Corner;\n"
    "    out.at = uv;\n"
    "    out.tint = color;\n"
    "    out.position = vec4f(p.xy, (p.z + p.w) * 0.5, p.w);\n" // The camera's clip space is OpenGL's: depth from -1
    "    return out;\n"
    "}\n"
    "@fragment fn fragment(in: Corner) -> @location(0) vec4f {\n"
    "    return textureSample(pixels, how, in.at) * in.tint;\n"
    "}\n");

// A clear: one triangle over the whole target, as far as depth goes, in one
// color, which the clip cuts as it does everything else.

static const char clear_glsl_vertex[] =
    "void main() {\n"
    "    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n" // (0, 0), (2, 0), (0, 2)
    "    gl_Position = vec4(corner * 2.0 - 1.0, 1.0, 1.0);\n"
    "}\n";

static const char clear_glsl_fragment[] =
    "layout(std140) uniform uniforms { vec4 color; };\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    pixel = color;\n"
    "}\n";

static const char *const clear_wgsl = WGSL(
    "struct Uniforms { color: vec4f }\n"
    "@group(0) @binding(0) var<uniform> u: Uniforms;\n"
    "@vertex fn vertex(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "    let corner = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4f(corner * 2.0 - 1.0, 1.0, 1.0);\n"
    "}\n"
    "@fragment fn fragment() -> @location(0) vec4f {\n"
    "    return u.color;\n"
    "}\n");
