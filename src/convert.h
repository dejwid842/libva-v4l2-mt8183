/*
 * Conversion helpers between V4L2 capture formats and the linear formats
 * exposed through VA-API.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <cstdint>

namespace mm21 {

// MM21 ("MediaTek 8-bit block mode") is plain 8-bit 4:2:0 NV12 data (no colour space change!),
// but stored in tiles that are 16 bytes wide:
//   - luma plane:   16x32 tiles
//   - chroma plane: 16x16 tiles (interleaved CbCr, so 8 chroma samples per tile row)
// Inside a tile, rows are stored one after another (16 bytes each). Tiles are stored left to right,
// and tile rows top to bottom, so a tile row occupies `stride * tile_height` bytes.

constexpr unsigned tile_width = 16;
constexpr unsigned luma_tile_height = 32;
constexpr unsigned chroma_tile_height = 16;

// Minimal size in bytes a plane has to have for `height` rows of `stride` bytes to be read safely.
// Tile rows are always stored in full, even if `height` does not end on a tile boundary.
unsigned long plane_min_size(unsigned stride, unsigned height, unsigned tile_height);

// Detile a single plane. `width` is in bytes, `src_stride` is the (untiled) row length in bytes
// and has to be a multiple of tile_width and at least `width` (rounded up to a tile).
void detile_plane(const uint8_t* src, unsigned src_stride, uint8_t* dst, unsigned dst_stride, unsigned width,
    unsigned height, unsigned tile_height, unsigned group_tiles = 1, bool row_major = false, unsigned prefetch_tiles = 0);

// Convert a whole MM21 frame (two separate planes) to linear NV12 (two planes, may share a buffer).
void to_nv12(const uint8_t* src_y, unsigned src_stride_y, const uint8_t* src_uv, unsigned src_stride_uv,
    uint8_t* dst_y, unsigned dst_stride_y, uint8_t* dst_uv, unsigned dst_stride_uv, unsigned width, unsigned height, unsigned group_tiles = 1, bool row_major = false, unsigned prefetch_tiles = 0);

} // namespace mm21
