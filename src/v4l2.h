/*
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2023 Max Schettler <max.schettler@posteo.de>
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
#include <optional>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
}

#define SOURCE_SIZE_MAX (1024 * 1024)

using fourcc = uint32_t;

class V4L2M2MDevice {
public:
    class Buffer {
    public:
        void queue(int request_fd = -1, timeval* timestamp = nullptr, unsigned size = 0) const;
        void dequeue() const;
        std::vector<int> export_(unsigned flags) const;
        std::vector<std::span<uint8_t>> mapping() const { return mapping_; }
        V4L2M2MDevice& owner() const { return owner_; }

        Buffer(V4L2M2MDevice& owner, v4l2_buf_type type, unsigned index);
        Buffer(Buffer&& other);
        Buffer& operator=(Buffer&& other);
        ~Buffer();

    private:
        V4L2M2MDevice& owner_;
        v4l2_buf_type type_;
        unsigned index_;
        std::vector<std::span<uint8_t>> mapping_;
        // The driver hands buffers back in the order it finished them, which isn't necessarily the order they're
        // asked for (e.g. B-frames). So the state of every buffer is tracked here.
        mutable bool queued_ = false;
        mutable bool error_ = false;

        friend class V4L2M2MDevice;
    };

    static std::vector<std::pair<std::string, std::optional<std::string>>> enumerate_devices();

    static const uint32_t required_capabilities = V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE;

    V4L2M2MDevice(const std::string& video_path, const std::optional<std::string>& media_path);
    V4L2M2MDevice(V4L2M2MDevice&& other);
    V4L2M2MDevice& operator=(V4L2M2MDevice&& other);
    ~V4L2M2MDevice();
    void set_format(enum v4l2_buf_type type, unsigned int pixelformat, unsigned int width, unsigned int height);
    // `cpu_cached` asks for cacheable (non-coherent) MMAP buffers, which makes CPU reads from them (as done for
    // software format conversion) much faster. Drivers not supporting cache hints silently ignore the request.
    unsigned request_buffers(enum v4l2_buf_type type, unsigned count, bool cpu_cached = false);
    // Adds buffers to those there are, without disturbing them (references to them stay valid). Returns how many were
    // added, which may be fewer than asked for. Throws if the driver doesn't allow it.
    unsigned create_buffers(enum v4l2_buf_type type, unsigned count, bool cpu_cached = false);
    unsigned buffer_count(v4l2_buf_type type) const;
    // The range of picture sizes the decoder handles for a coded format, as it reports it (VIDIOC_ENUM_FRAMESIZES).
    // Not every size within the limits is usable (see the step), but nothing outside of them is.
    struct FrameSizeLimits {
        struct Range {
            unsigned min_width, min_height, max_width, max_height;
            unsigned step_width, step_height;
        };

        // The envelope is useful for VAConfigAttribMaxPictureWidth/Height. Exact support must be
        // checked against the discrete sizes and stepwise/continuous ranges below.
        unsigned min_width = 0, min_height = 0, max_width = 0, max_height = 0;
        std::vector<std::pair<unsigned, unsigned>> discrete_sizes;
        std::vector<Range> ranges;

        bool supports(unsigned width, unsigned height) const
        {
            for (const auto& size : discrete_sizes) {
                if (size.first == width && size.second == height) {
                    return true;
                }
            }
            for (const auto& range : ranges) {
                if (width < range.min_width || width > range.max_width || height < range.min_height
                    || height > range.max_height) {
                    continue;
                }
                if ((width - range.min_width) % range.step_width == 0
                    && (height - range.min_height) % range.step_height == 0) {
                    return true;
                }
            }
            return false;
        }
    };
    std::optional<FrameSizeLimits> frame_size_limits(unsigned pixelformat) const;

    bool format_supported(v4l2_buf_type type, unsigned pixelformat) const;
    const Buffer& buffer(v4l2_buf_type type, unsigned index);
    int32_t get_control(uint32_t id) const;
    void set_ext_control(int request_fd, unsigned id, void* data, unsigned size);
    void set_ext_controls(int request_fd, std::span<v4l2_ext_control> controls);
    void set_streaming(bool enable);

    // Where the device was opened from, to be able to open it again: every user (context) needs its own instance of
    // a decoder, which is what opening the device gives.
    std::string video_path;
    std::optional<std::string> media_path;

    int video_fd;
    int media_fd;
    const uint32_t capabilities;
    const v4l2_buf_type capture_buf_type;
    const v4l2_buf_type output_buf_type;
    // Re-reads the capture format from the driver, which may have changed it since it was set (e.g. after parsing
    // the stream headers), and returns a human readable description of it (for diagnostics).
    std::string refresh_capture_format();
    std::string describe_format(v4l2_buf_type type) const;

    v4l2_format capture_format;
    v4l2_format output_format;
    std::set<fourcc> supported_output_formats;
    std::set<fourcc> supported_capture_formats;

private:
    // (A deque keeps the buffers where they are when more are added.)
    std::deque<Buffer> capture_buffers;
    std::deque<Buffer> output_buffers;
    // The set of buffers can change while others look at it (see create_buffers()). Shared, so that the device stays movable.
    std::shared_ptr<std::mutex> buffers_mutex = std::make_shared<std::mutex>();
};
