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
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <linux/videodev2.h>

/**
 * Converts decoded MM21 pictures (MediaTek's tiled NV12) to linear NV12 with the MDP3 hardware (the `mtk-mdp3`
 * memory-to-memory device), instead of the CPU.
 *
 * The converted pictures live in buffers of that device, which can be exported as DMA-BUFs: no dma-heap or udmabuf is
 * needed for that. The decoder's own buffers are handed to MDP3 as DMA-BUFs, so nothing is copied by the CPU.
 */
class Mdp3Converter {
public:
    struct Result {
        int fd; // DMA-BUF of the converted picture, owned by the converter (duplicate it to hand it out)
        unsigned pitch; // of both planes
        unsigned uv_offset; // where the chroma plane starts
        unsigned size; // of the whole buffer
    };

    // Returns null, and says why in `error`, if there's no usable MDP3 device.
    static std::unique_ptr<Mdp3Converter> create(
        const v4l2_pix_format_mplane& source_format, unsigned buffer_count, std::string& error);
    ~Mdp3Converter();

    // A DMA-BUF holding an (empty) NV12 picture of the given size, made by the MDP3 driver: for surfaces nothing has
    // been decoded into yet, but which the application wants to export anyway (players do, to probe what works).
    // `fd` of the result is owned by the caller. Throws std::runtime_error if that isn't possible.
    static Result allocate_nv12(unsigned width, unsigned height);

    // Converts the picture whose two MM21 planes are the given DMA-BUFs (and their sizes), into buffer `index`.
    Result convert(unsigned index, std::span<const int> source_fds, std::span<const size_t> source_sizes);

    unsigned buffer_count() const { return count_; }

private:
    explicit Mdp3Converter(int video_fd)
        : fd_(video_fd)
    {
    }
    void setup(const v4l2_pix_format_mplane& source_format, unsigned buffer_count);
    bool dequeue(v4l2_buf_type type, v4l2_memory memory, unsigned planes);
    void restart();

    int fd_;
    unsigned count_ = 0;
    unsigned source_sizeimage_[2] = {};
    unsigned pitch_ = 0, uv_offset_ = 0, size_ = 0;
    std::vector<int> capture_fds_;
    std::mutex mutex_;
};
