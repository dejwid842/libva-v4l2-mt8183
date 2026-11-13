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
 * A buffer allocated by a display driver (a DRM "dumb buffer", e.g. of mediatek-drm), which the CPU can write to and
 * which can be exported as a DMA-BUF. Needs no dma-heap or udmabuf in the kernel, only access to /dev/dri/card*.
 *
 * Such memory is typically write-combined: fine for writing a picture into it (and for the GPU to read), but slow for
 * the CPU to read back, which is why it is used for exporting only.
 */
class DrmBuffer {
public:
    // Throws std::runtime_error (saying what was tried) if no DRM device is able to allocate one.
    static DrmBuffer allocate(size_t size);

    DrmBuffer(const DrmBuffer&) = delete;
    DrmBuffer& operator=(const DrmBuffer&) = delete;
    DrmBuffer(DrmBuffer&& other) noexcept;
    ~DrmBuffer();

    uint8_t* data() const { return map_; }
    size_t size() const { return size_; }

    // New descriptor of the DMA-BUF, ownership is passed to the caller.
    int duplicate_fd() const;

private:
    DrmBuffer(int drm_fd, uint32_t handle, uint8_t* map, size_t size, int dmabuf_fd)
        : drm_fd_(drm_fd)
        , handle_(handle)
        , map_(map)
        , size_(size)
        , dmabuf_fd_(dmabuf_fd)
    {
    }

    int drm_fd_ = -1;
    uint32_t handle_ = 0;
    uint8_t* map_ = nullptr;
    size_t size_ = 0;
    int dmabuf_fd_ = -1;
};
