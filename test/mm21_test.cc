// Standalone test of the MM21 -> NV12 conversion (no libva / V4L2 hardware required).
//
//   g++ -std=c++20 -Wall -O2 -Isrc test/mm21_test.cc src/convert.cc src/dmabuf.cc -o mm21_test && ./mm21_test
//
// The "reference" tiler below is intentionally written differently from the production code (per-pixel offset
// formula instead of tile-by-tile copies), so a mistake in one is unlikely to be mirrored in the other.
// Note: this verifies self-consistency with the layout described in convert.h. Whether that layout matches
// what the MT8183 hardware writes has to be verified on the device (see README).

#include "convert.h"
#include "dmabuf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

unsigned align_up(unsigned v, unsigned a) { return (v + a - 1) / a * a; }

// Offset of byte (x, y) of a plane stored as 16-byte-wide tiles of `th` rows.
size_t tiled_offset(unsigned x, unsigned y, unsigned stride, unsigned th)
{
    const size_t tile_row = y / th, tile_col = x / 16;
    return tile_row * stride * th + tile_col * 16 * th + (y % th) * 16 + (x % 16);
}

bool run_case(unsigned width, unsigned height, unsigned stride_extra, std::mt19937& rng)
{
    const unsigned stride = align_up(width, 16) + stride_extra; // stride_extra must be a multiple of 16
    const unsigned uv_rows = (height + 1) / 2;
    const unsigned uv_width = (width + 1) & ~1u;

    // Linear reference frame
    std::vector<uint8_t> ref_y(static_cast<size_t>(width) * height), ref_uv(static_cast<size_t>(uv_width) * uv_rows);
    for (auto& v : ref_y) v = rng();
    for (auto& v : ref_uv) v = rng();

    // Tile it (plane buffers are padded to whole tile rows, like a real driver allocates them)
    std::vector<uint8_t> tiled_y(mm21::plane_min_size(stride, height, 32), 0xEE);
    std::vector<uint8_t> tiled_uv(mm21::plane_min_size(stride, uv_rows, 16), 0xEE);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            tiled_y[tiled_offset(x, y, stride, 32)] = ref_y[static_cast<size_t>(y) * width + x];
    for (unsigned y = 0; y < uv_rows; y++)
        for (unsigned x = 0; x < uv_width; x++)
            tiled_uv[tiled_offset(x, y, stride, 16)] = ref_uv[static_cast<size_t>(y) * uv_width + x];

    // Detile into a buffer with a different (aligned) pitch, as the real destination has
    const unsigned pitch = align_up(width, 64);
    std::vector<uint8_t> out(static_cast<size_t>(pitch) * (height + uv_rows), 0xAA);
    // Compare each grouped traversal with the independent per-pixel reference.
    for (unsigned group : {1u, 4u, 8u}) for (unsigned prefetch : {0u, 1u, 2u}) {
    std::fill(out.begin(), out.end(), 0xAA);
    mm21::to_nv12(tiled_y.data(), stride, tiled_uv.data(), stride, out.data(), pitch,
        out.data() + static_cast<size_t>(pitch) * height, pitch, width, height, group, false, prefetch);

    for (unsigned y = 0; y < height; y++)
        if (std::memcmp(&out[static_cast<size_t>(y) * pitch], &ref_y[static_cast<size_t>(y) * width], width)) {
            std::printf("FAIL %ux%u (stride %u): luma row %u differs\n", width, height, stride, y);
            return false;
        }
    for (unsigned y = 0; y < uv_rows; y++)
        if (std::memcmp(&out[static_cast<size_t>(pitch) * height + static_cast<size_t>(y) * pitch],
                &ref_uv[static_cast<size_t>(y) * uv_width], uv_width)) {
            std::printf("FAIL %ux%u (stride %u): chroma row %u differs\n", width, height, stride, y);
            return false;
        }

    // Padding between width and pitch must be untouched
    if (width < pitch && out[width] != 0xAA) {
        std::printf("FAIL %ux%u: wrote past the requested width\n", width, height);
        return false;
    }

    } // group
    std::printf("ok   %ux%u (stride %u; groups 1,4,8)\n", width, height, stride);
    return true;
}

} // namespace

int main()
{
    std::mt19937 rng(1234);
    bool ok = true;

    const unsigned sizes[][3] = {
        { 16, 32, 0 }, { 32, 32, 0 }, { 64, 64, 0 }, { 1920, 1088, 0 }, { 1920, 1080, 0 },
        { 1088, 1920, 0 }, { 1080, 1920, 0 }, { 1280, 720, 0 },
        { 854, 480, 0 }, { 854, 480, 32 }, { 1366, 768, 16 }, { 100, 50, 0 }, { 17, 33, 16 }, { 1, 1, 0 }, { 33, 17, 0 },
    };
    for (const auto& s : sizes) {
        ok &= run_case(s[0], s[1], s[2], rng);
    }

    // Allocation needs /dev/dma_heap or /dev/udmabuf access, which is not available everywhere: only a smoke test.
    try {
        auto buffer = DmaBuffer::allocate(1 << 20);
        buffer.begin_cpu_access();
        std::memset(buffer.data(), 0x5A, buffer.size());
        buffer.end_cpu_access();
        if (buffer.exportable()) {
            std::printf("ok   DmaBuffer allocate/duplicate (fd %d)\n", buffer.duplicate_fd());
        } else {
            std::printf("ok   DmaBuffer allocate (plain memory fallback, not exportable)\n");
        }
        DmaBuffer moved(std::move(buffer));
        if (buffer.fd() != -1 || moved.data()[0] != 0x5A) {
            std::printf("FAIL DmaBuffer move\n");
            ok = false;
        }
    } catch (std::exception& e) {
        std::printf("skip DmaBuffer (no allocator available here): %s\n", e.what());
    }

    std::printf(ok ? "ALL PASSED\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
