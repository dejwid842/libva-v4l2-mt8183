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

#pragma once

#include <cstddef>
#include <cstdint>

/**
 * A CPU-mappable DMA-BUF, used as the target of software format conversions, so that the result can still be
 * handed out as a DRM PRIME handle (vaExportSurfaceHandle).
 *
 * Allocation is attempted from a dma-heap (`/dev/dma_heap/system` by default, override with the
 * LIBVA_V4L2_DMA_HEAP environment variable, either a heap name or a full path), falling back to udmabuf.
 * If neither is available (kernel without CONFIG_DMABUF_HEAPS / CONFIG_UDMABUF) plain shared memory is used.
 * Such a buffer works for CPU access (vaGetImage, vaDeriveImage), but can't be exported (see exportable()).
 */
class DmaBuffer {
public:
    // Throws std::runtime_error / std::system_error if no allocator is usable. With `dmabuf_only`, plain memory is not
    // an acceptable result (it throws instead).
    static DmaBuffer allocate(size_t size, bool dmabuf_only = false);

    // Maps a DMA-BUF the application provided (the descriptor is duplicated, the caller keeps its own). `size` may be 0
    // to use the size of the DMA-BUF. Throws std::runtime_error if it can't be mapped for writing.
    static DmaBuffer import(int fd, size_t size);

    DmaBuffer() = default;
    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;
    DmaBuffer(DmaBuffer&& other) noexcept;
    DmaBuffer& operator=(DmaBuffer&& other) noexcept;
    ~DmaBuffer();

    int fd() const { return fd_; }
    size_t size() const { return size_; }
    uint8_t* data() const { return map_; }

    // Whether this is a real DMA-BUF, that other devices (GPU, display) can import.
    bool exportable() const { return exportable_; }

    // New descriptor for the same buffer; ownership is passed to the caller. Throws if !exportable().
    int duplicate_fd() const;

    // CPU access has to be bracketed, or the GPU/display might see stale data on non-coherent systems.
    void begin_cpu_access(bool write = true) const;
    void end_cpu_access(bool write = true) const;
    // VAImage mappings can be used for either reading or writing; synchronize both directions.
    void begin_cpu_access_rw() const;
    void end_cpu_access_rw() const;

private:
    DmaBuffer(int fd, size_t size, uint8_t* map, bool exportable)
        : fd_(fd)
        , size_(size)
        , map_(map)
        , exportable_(exportable)
    {
    }

    int fd_ = -1;
    size_t size_ = 0;
    uint8_t* map_ = nullptr;
    bool exportable_ = false;
};
