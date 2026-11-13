/*
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

#include "convert.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace mm21 {

unsigned long plane_min_size(unsigned stride, unsigned height, unsigned tile_height)
{
    const unsigned long tile_rows = (height + tile_height - 1) / tile_height;
    return tile_rows * tile_height * static_cast<unsigned long>(stride);
}

void detile_plane(const uint8_t* src, unsigned src_stride, uint8_t* dst, unsigned dst_stride, unsigned width,
    unsigned height, unsigned tile_height, unsigned group_tiles, bool row_major, unsigned prefetch_tiles)
{
    const size_t tile_size = static_cast<size_t>(tile_width) * tile_height;
    const size_t tile_row_size = static_cast<size_t>(src_stride) * tile_height;
    // group_tiles=1 keeps the original traversal. Grouped mode improves destination
    // row locality: several adjacent 16-byte spans are written before advancing
    // to the next row. The entire source group is small (at most 4 KiB).
    if (group_tiles != 4 && group_tiles != 8) group_tiles = 1;
    if (prefetch_tiles > 2) prefetch_tiles = 0;
    for (unsigned ty = 0; ty < height; ty += tile_height) {
        const unsigned rows = std::min(tile_height, height - ty);
        const uint8_t* tile_row = src + (ty / tile_height) * tile_row_size;
        if (row_major) {
            // Scan the output band in increasing linear destination addresses.
            // Each source tile is 16 bytes wide and tile_height rows tall.
            // Preserve partial tiles and partial bottom bands exactly.
            for (unsigned r = 0; r < rows; ++r) {
                uint8_t* line = dst + static_cast<size_t>(ty + r) * dst_stride;
                for (unsigned tx = 0; tx < width; tx += tile_width) {
                    const unsigned bytes = std::min(tile_width, width - tx);
                    const uint8_t* tile = tile_row
                        + static_cast<size_t>(tx / tile_width) * tile_size;
                    std::memcpy(line + tx, tile + static_cast<size_t>(r) * tile_width, bytes);
                }
            }
            continue;
        }
        for (unsigned tx = 0; tx < width; tx += tile_width * group_tiles) {
            const unsigned tiles = std::min(group_tiles, (width - tx + tile_width - 1) / tile_width);
            uint8_t* out = dst + static_cast<size_t>(ty) * dst_stride + tx;
            const uint8_t* group = tile_row + (tx / tile_width) * tile_size;
#if defined(__GNUC__) || defined(__clang__)
            // Prefetch a *future source tile*, never the destination DMA-BUF.
            // Only issue hints within the currently addressed tile band.
            // This intentionally does not change the order of MM21 reads/writes.
            if (prefetch_tiles != 0) {
                const unsigned future = tx / tile_width + prefetch_tiles;
                const unsigned source_tiles = (width + tile_width - 1) / tile_width;
                if (future < source_tiles) {
                    const uint8_t* ahead = tile_row + static_cast<size_t>(future) * tile_size;
                    __builtin_prefetch(ahead, 0, 1);
                    // For a 16x32 luma tile, the second half is in separate lines.
                    if (tile_size > 256) __builtin_prefetch(ahead + tile_size / 2, 0, 1);
                }
            }
#endif
            for (unsigned r = 0; r < rows; ++r) {
                uint8_t* line = out + static_cast<size_t>(r) * dst_stride;
                for (unsigned t = 0; t < tiles; ++t) {
                    const unsigned x = tx + t * tile_width;
                    const unsigned bytes = std::min(tile_width, width - x);
                    std::memcpy(line + t * tile_width,
                        group + static_cast<size_t>(t) * tile_size + r * tile_width, bytes);
                }
            }
        }
    }
}

void to_nv12(const uint8_t* src_y, unsigned src_stride_y, const uint8_t* src_uv, unsigned src_stride_uv,
    uint8_t* dst_y, unsigned dst_stride_y, uint8_t* dst_uv, unsigned dst_stride_uv, unsigned width, unsigned height,
    unsigned group_tiles, bool row_major, unsigned prefetch_tiles)
{
    detile_plane(src_y, src_stride_y, dst_y, dst_stride_y, width, height, luma_tile_height, group_tiles, row_major, prefetch_tiles);
    detile_plane(src_uv, src_stride_uv, dst_uv, dst_stride_uv, (width + 1) & ~1u, (height + 1) / 2,
        chroma_tile_height, group_tiles, row_major, prefetch_tiles);
}

} // namespace mm21
