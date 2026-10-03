#pragma once

#include <stdint.h>

#include "tide/draw.h"
#include "tide/grid.h"
#include "tide/list.h"

// Tide's Draw.Mesh, as generated code calls it: lists of `Vertex` and of int,
// and a `Grid2<Color>` to draw with, whose cell (0, 0) is the texture's first
// pixel.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// The grid is the texture: nothing else holds its pixels. The draw list copies
// its cells, a byte a channel, when they aren't what it last copied, which the
// hashes the grid's heap keeps of its pages tell without reading the cells
// (tide_heap_block_hash). A grid with no size on an axis is no texture: the
// mesh draws with its colors alone.

void tide_draw_mesh_lists(tide_draw_list *d, tide_list vertices, tide_list indices);
void tide_draw_mesh_grid(tide_draw_list *d, tide_list vertices, tide_list indices, tide_grid texture,
                         const tide_grid_shape *shape, int32_t filter);
