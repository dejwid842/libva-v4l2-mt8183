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


#include "mdp3.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

int checked_ioctl(int fd, unsigned long request, void* argument, const char* what)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), what);
    }
    return result;
}

std::string describe(const v4l2_pix_format_mplane& format)
{
    std::string text = std::to_string(format.width) + "x" + std::to_string(format.height) + " '"
        + std::string(reinterpret_cast<const char*>(&format.pixelformat), 4) + "', "
        + std::to_string(format.num_planes) + " planes:";
    for (unsigned i = 0; i < format.num_planes && i < VIDEO_MAX_PLANES; i++) {
        text += " [bytesperline " + std::to_string(format.plane_fmt[i].bytesperline) + ", sizeimage "
            + std::to_string(format.plane_fmt[i].sizeimage) + "]";
    }
    return text;
}

// The MDP3 is a plain memory-to-memory device (there's no media device to find it by), so look at all of them.
int open_mdp3(std::string& error)
{
    std::vector<std::string> paths;
    if (const char* forced = getenv("LIBVA_V4L2_MDP3_PATH"); forced && *forced) {
        paths.push_back(forced);
    } else {
        glob_t found = {};
        if (glob("/dev/video*", 0, nullptr, &found) == 0) {
            for (size_t i = 0; i < found.gl_pathc; i++) {
                paths.push_back(found.gl_pathv[i]);
            }
        }
        globfree(&found);
    }

    for (const auto& path : paths) {
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        v4l2_capability capability = {};
        if (ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0 && strcmp(reinterpret_cast<const char*>(capability.driver), "mtk-mdp3") == 0) {
            const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
            if (caps & V4L2_CAP_VIDEO_M2M_MPLANE) {
                return fd;
            }
        }
        close(fd);
    }

    error = "no MediaTek MDP3 device (driver mtk-mdp3) found";
    return -1;
}

} // namespace

std::unique_ptr<Mdp3Converter> Mdp3Converter::create(
    const v4l2_pix_format_mplane& source_format, unsigned buffer_count, std::string& error)
{
    const int fd = open_mdp3(error);
    if (fd < 0) {
        return nullptr;
    }

    std::unique_ptr<Mdp3Converter> converter(new Mdp3Converter(fd));
    try {
        converter->setup(source_format, buffer_count);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    return converter;
}

void Mdp3Converter::setup(const v4l2_pix_format_mplane& source_format, unsigned buffer_count)
{
    if (source_format.num_planes != 2) {
        throw std::runtime_error("the decoder's pictures don't consist of two planes");
    }

    // What goes in: the decoder's pictures, in the same layout they have there.
    v4l2_format input = {};
    input.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    input.fmt.pix_mp.width = source_format.width;
    input.fmt.pix_mp.height = source_format.height;
    input.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MM21;
    input.fmt.pix_mp.field = V4L2_FIELD_NONE;
    input.fmt.pix_mp.num_planes = 2;
    for (unsigned i = 0; i < 2; i++) {
        input.fmt.pix_mp.plane_fmt[i].bytesperline = source_format.plane_fmt[i].bytesperline;
        input.fmt.pix_mp.plane_fmt[i].sizeimage = source_format.plane_fmt[i].sizeimage;
    }
    checked_ioctl(fd_, VIDIOC_S_FMT, &input, "MDP3: setting the source format");

    const auto& accepted = input.fmt.pix_mp;
    if (accepted.pixelformat != V4L2_PIX_FMT_MM21 || accepted.num_planes != 2) {
        throw std::runtime_error("MDP3 doesn't take the decoder's format, it chose " + describe(accepted));
    }
    for (unsigned i = 0; i < 2; i++) {
        source_sizeimage_[i] = accepted.plane_fmt[i].sizeimage;
        if (accepted.plane_fmt[i].sizeimage > source_format.plane_fmt[i].sizeimage) {
            throw std::runtime_error("MDP3 expects bigger source planes than the decoder's: " + describe(accepted)
                + " against " + describe(source_format));
        }
    }

    // What comes out: plain NV12 in a single buffer (so that it is a single DMA-BUF, too).
    v4l2_format output = {};
    output.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    output.fmt.pix_mp.width = accepted.width;
    output.fmt.pix_mp.height = accepted.height;
    output.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    output.fmt.pix_mp.field = V4L2_FIELD_NONE;
    output.fmt.pix_mp.num_planes = 1;
    checked_ioctl(fd_, VIDIOC_S_FMT, &output, "MDP3: setting the result format");

    const auto& produced = output.fmt.pix_mp;
    if (produced.pixelformat != V4L2_PIX_FMT_NV12 || produced.num_planes != 1) {
        throw std::runtime_error("MDP3 doesn't produce single buffer NV12, it chose " + describe(produced));
    }
    pitch_ = produced.plane_fmt[0].bytesperline;
    size_ = produced.plane_fmt[0].sizeimage;
    uv_offset_ = pitch_ * produced.height;
    if (pitch_ == 0 || size_ < uv_offset_ + pitch_ * ((produced.height + 1) / 2)) {
        throw std::runtime_error("MDP3's NV12 has an unexpected layout: " + describe(produced));
    }

    // Buffers: the decoder's come in as DMA-BUFs, ours are allocated by MDP3.
    v4l2_requestbuffers request = {};
    request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    request.memory = V4L2_MEMORY_DMABUF;
    request.count = buffer_count;
    checked_ioctl(fd_, VIDIOC_REQBUFS, &request, "MDP3: requesting source buffers");
    const unsigned source_count = request.count;

    request = {};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = buffer_count;
    checked_ioctl(fd_, VIDIOC_REQBUFS, &request, "MDP3: requesting result buffers");

    count_ = std::min(source_count, request.count);
    if (count_ == 0) {
        throw std::runtime_error("MDP3 gave no buffers");
    }

    for (unsigned i = 0; i < count_; i++) {
        v4l2_exportbuffer exported = {};
        exported.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        exported.index = i;
        exported.plane = 0;
        exported.flags = O_RDWR | O_CLOEXEC;
        checked_ioctl(fd_, VIDIOC_EXPBUF, &exported, "MDP3: exporting a result buffer");
        capture_fds_.push_back(exported.fd);
    }

    for (const auto type : { V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE }) {
        int streaming_type = type;
        checked_ioctl(fd_, VIDIOC_STREAMON, &streaming_type, "MDP3: starting");
    }
}

Mdp3Converter::Result Mdp3Converter::allocate_nv12(unsigned width, unsigned height)
{
    std::string error;
    const int fd = open_mdp3(error);
    if (fd < 0) {
        throw std::runtime_error(error);
    }
    // (Closing the device frees its buffers, but not what has been exported: that lives on as long as the DMA-BUF.)
    struct Closer {
        int fd;
        ~Closer() { close(fd); }
    } closer { fd };

    try {
        v4l2_format format = {};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        format.fmt.pix_mp.width = width;
        format.fmt.pix_mp.height = height;
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        format.fmt.pix_mp.num_planes = 1;
        checked_ioctl(fd, VIDIOC_S_FMT, &format, "MDP3: setting the format");

        const auto& produced = format.fmt.pix_mp;
        if (produced.pixelformat != V4L2_PIX_FMT_NV12 || produced.num_planes != 1) {
            throw std::runtime_error("MDP3 doesn't produce single buffer NV12, it chose " + describe(produced));
        }

        v4l2_requestbuffers request = {};
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        request.memory = V4L2_MEMORY_MMAP;
        request.count = 1;
        checked_ioctl(fd, VIDIOC_REQBUFS, &request, "MDP3: requesting a buffer");
        if (request.count < 1) {
            throw std::runtime_error("MDP3 gave no buffer");
        }

        v4l2_exportbuffer exported = {};
        exported.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        exported.index = 0;
        exported.plane = 0;
        exported.flags = O_RDWR | O_CLOEXEC;
        checked_ioctl(fd, VIDIOC_EXPBUF, &exported, "MDP3: exporting a buffer");

        const unsigned pitch = produced.plane_fmt[0].bytesperline;
        return Result { exported.fd, pitch, pitch * produced.height, produced.plane_fmt[0].sizeimage };
    } catch (const std::system_error& e) {
        throw std::runtime_error(e.what());
    }
}

Mdp3Converter::~Mdp3Converter()
{
    for (const int fd : capture_fds_) {
        close(fd);
    }
    if (fd_ >= 0) {
        close(fd_); // (which also stops streaming and frees the buffers)
    }
}

// Takes the one buffer that is done off the queue, if it is done. Throws if the device flagged an error.
bool Mdp3Converter::dequeue(v4l2_buf_type type, v4l2_memory memory, unsigned planes)
{
    v4l2_plane plane_array[VIDEO_MAX_PLANES] = {};
    v4l2_buffer buffer = {};
    buffer.type = type;
    buffer.memory = memory;
    buffer.length = planes;
    buffer.m.planes = plane_array;

    if (ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return false;
        }
        throw std::system_error(errno, std::generic_category(), "MDP3: dequeueing");
    }
    if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
        throw std::runtime_error("MDP3 reported an error for the conversion");
    }
    return true;
}

// After a failure there may be buffers stuck in the queues: starting over is the way to get them back.
void Mdp3Converter::restart()
{
    for (const auto type : { V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE }) {
        int streaming_type = type;
        ioctl(fd_, VIDIOC_STREAMOFF, &streaming_type);
        ioctl(fd_, VIDIOC_STREAMON, &streaming_type);
    }
}

Mdp3Converter::Result Mdp3Converter::convert(
    unsigned index, std::span<const int> source_fds, std::span<const size_t> source_sizes)
{
    if (source_fds.size() != 2 || source_sizes.size() != 2) {
        throw std::invalid_argument("MDP3 needs the two planes of a picture");
    }

    std::lock_guard<std::mutex> guard(mutex_);
    if (index >= count_) {
        throw std::out_of_range("MDP3 has no buffer for this picture");
    }

    try {
        v4l2_plane source_planes[VIDEO_MAX_PLANES] = {};
        for (unsigned i = 0; i < 2; i++) {
            source_planes[i].m.fd = source_fds[i];
            source_planes[i].length = source_sizes[i];
            source_planes[i].bytesused = source_sizeimage_[i];
        }
        v4l2_buffer source = {};
        source.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        source.memory = V4L2_MEMORY_DMABUF;
        source.index = index;
        source.length = 2;
        source.m.planes = source_planes;
        checked_ioctl(fd_, VIDIOC_QBUF, &source, "MDP3: queueing the decoded picture");

        v4l2_plane result_planes[VIDEO_MAX_PLANES] = {};
        v4l2_buffer result = {};
        result.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        result.memory = V4L2_MEMORY_MMAP;
        result.index = index;
        result.length = 1;
        result.m.planes = result_planes;
        checked_ioctl(fd_, VIDIOC_QBUF, &result, "MDP3: queueing the result buffer");

        // The result is ready when the device becomes readable.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                throw std::runtime_error("MDP3 didn't finish in time");
            }
            pollfd waiting = { fd_, POLLIN | POLLRDNORM, 0 };
            const int ready = poll(&waiting, 1, static_cast<int>(remaining));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                throw std::runtime_error("MDP3 didn't finish in time");
            }
            if (dequeue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, 1)) {
                break;
            }
        }

        // The source buffer is released at the same time (give it a moment, should it lag behind).
        bool released = false;
        for (int attempt = 0; attempt < 200 && !released; attempt++) {
            released = dequeue(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_DMABUF, 2);
            if (!released) {
                usleep(100);
            }
        }
        if (!released) {
            throw std::runtime_error("MDP3 didn't release the decoded picture");
        }
    } catch (...) {
        restart();
        throw;
    }

    return Result { capture_fds_[index], pitch_, uv_offset_, size_ };
}
