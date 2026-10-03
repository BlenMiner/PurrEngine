// WebGPU under the renderer (see src/gpu.h): the page's (tide.js), which has
// each of these as an import. The program draws with it when the page found a
// device before it started (tide_web_init_canvas says), else with WebGL 2.

#include "gpu.h"

#include "tide_web.h"

TIDE_WEB_IMPORT(gpu_buffer) tide_gpu_id tide_web_gpu_buffer(tide_gpu_buffer_kind kind, size_t size, const void *data);
TIDE_WEB_IMPORT(gpu_buffer_write) void tide_web_gpu_buffer_write(tide_gpu_id buffer, size_t offset, const void *data, size_t size);
TIDE_WEB_IMPORT(gpu_buffer_free) void tide_web_gpu_buffer_free(tide_gpu_id buffer);
TIDE_WEB_IMPORT(gpu_texture) tide_gpu_id tide_web_gpu_texture(tide_gpu_format format, int width, int height, const void *pixels);
TIDE_WEB_IMPORT(gpu_texture_write) void tide_web_gpu_texture_write(tide_gpu_id texture, const void *pixels);
TIDE_WEB_IMPORT(gpu_texture_free) void tide_web_gpu_texture_free(tide_gpu_id texture);
// A pipeline, piece by piece: its slots and attributes in order, between begin and end
TIDE_WEB_IMPORT(gpu_pipeline_begin) void tide_web_gpu_pipeline_begin(const char *name, const char *wgsl, uint32_t uniform_size,
                                                             bool texture, tide_gpu_blend blend, tide_gpu_depth depth);
TIDE_WEB_IMPORT(gpu_pipeline_slot) void tide_web_gpu_pipeline_slot(uint32_t stride, bool per_instance);
TIDE_WEB_IMPORT(gpu_pipeline_attribute) void tide_web_gpu_pipeline_attribute(uint32_t slot, tide_gpu_attribute_format format,
                                                                     uint32_t offset);
TIDE_WEB_IMPORT(gpu_pipeline_end) tide_gpu_id tide_web_gpu_pipeline_end(void);
TIDE_WEB_IMPORT(gpu_target) tide_gpu_id tide_web_gpu_target(int width, int height);
TIDE_WEB_IMPORT(gpu_target_free) void tide_web_gpu_target_free(tide_gpu_id target);
TIDE_WEB_IMPORT(gpu_target_read) void tide_web_gpu_target_read(tide_gpu_id target, uint8_t *rgba);
TIDE_WEB_IMPORT(gpu_begin) void tide_web_gpu_begin(tide_gpu_id target, bool clear);
TIDE_WEB_IMPORT(gpu_end) void tide_web_gpu_end(void);
TIDE_WEB_IMPORT(gpu_scissor) void tide_web_gpu_scissor(int x, int y, int width, int height);
TIDE_WEB_IMPORT(gpu_use) void tide_web_gpu_use(tide_gpu_id pipeline);
TIDE_WEB_IMPORT(gpu_uniforms) void tide_web_gpu_uniforms(const void *data, size_t size);
TIDE_WEB_IMPORT(gpu_sample) void tide_web_gpu_sample(tide_gpu_id texture, bool blended);
TIDE_WEB_IMPORT(gpu_vertices) void tide_web_gpu_vertices(uint32_t slot, tide_gpu_id buffer, size_t offset);
TIDE_WEB_IMPORT(gpu_indices) void tide_web_gpu_indices(tide_gpu_id buffer, bool wide);
TIDE_WEB_IMPORT(gpu_draw) void tide_web_gpu_draw(uint32_t first, uint32_t count, uint32_t instances);
TIDE_WEB_IMPORT(gpu_draw_indexed) void tide_web_gpu_draw_indexed(uint32_t count, uint32_t instances);

static tide_gpu_id tide_web_gpu_pipeline(const tide_gpu_pipeline_desc *desc)
{
    tide_web_gpu_pipeline_begin(desc->name, desc->wgsl, desc->uniform_size, desc->texture, desc->blend, desc->depth);
    for (uint32_t i = 0; i < desc->slot_count; i++) tide_web_gpu_pipeline_slot(desc->slots[i].stride, desc->slots[i].per_instance);
    for (uint32_t i = 0; i < desc->attribute_count; i++) {
        tide_web_gpu_pipeline_attribute(desc->attributes[i].slot, desc->attributes[i].format, desc->attributes[i].offset);
    }
    return tide_web_gpu_pipeline_end();
}

const tide_gpu tide_gpu_webgpu = {
    .name = "WebGPU",
    .buffer = tide_web_gpu_buffer,
    .buffer_write = tide_web_gpu_buffer_write,
    .buffer_free = tide_web_gpu_buffer_free,
    .texture = tide_web_gpu_texture,
    .texture_write = tide_web_gpu_texture_write,
    .texture_free = tide_web_gpu_texture_free,
    .pipeline = tide_web_gpu_pipeline,
    .target = tide_web_gpu_target,
    .target_free = tide_web_gpu_target_free,
    .target_read = tide_web_gpu_target_read,
    .begin = tide_web_gpu_begin,
    .end = tide_web_gpu_end,
    .scissor = tide_web_gpu_scissor,
    .use = tide_web_gpu_use,
    .uniforms = tide_web_gpu_uniforms,
    .sample = tide_web_gpu_sample,
    .vertices = tide_web_gpu_vertices,
    .indices = tide_web_gpu_indices,
    .draw = tide_web_gpu_draw,
    .draw_indexed = tide_web_gpu_draw_indexed,
};
