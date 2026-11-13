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

#include "v4l2.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

extern "C" {
#include <fcntl.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <libudev.h>
}

#include "utils.h"

namespace {

uint32_t query_capabilities(int video_fd)
{
    v4l2_capability capability = {};
    errno_wrapper(ioctl, video_fd, VIDIOC_QUERYCAP, &capability);

    if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0) {
        return capability.device_caps;
    } else {
        return capability.capabilities;
    }
}

v4l2_format get_format(int video_fd, v4l2_buf_type type)
{
    v4l2_format result = { .type = type };
    errno_wrapper(ioctl, video_fd, VIDIOC_G_FMT, &result);
    return result;
}

std::vector<std::span<uint8_t>> map_buffer(int video_fd, v4l2_buf_type type, unsigned index)
{
    v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    v4l2_buffer buffer = {
        .index = index,
        .type = type,
        .m = { .planes = planes },
        .length = VIDEO_MAX_PLANES,
    };
    errno_wrapper(ioctl, video_fd, VIDIOC_QUERYBUF, &buffer);

    if (!V4L2_TYPE_IS_MULTIPLANAR(type)) { // reduce singleplanar API to single-plane in multiplanar buffer
        const auto offset = buffer.m.offset;
        buffer.m.planes = planes;
        buffer.m.planes[0].length = buffer.length;
        buffer.m.planes[0].m.mem_offset = offset;
        buffer.length = 1;
    }

    std::vector<std::span<uint8_t>> result(buffer.length);
    for (unsigned i = 0; i < buffer.length; i++) {
        result[i] = { static_cast<uint8_t*>(mmap(NULL, buffer.m.planes[i].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                          video_fd, buffer.m.planes[i].m.mem_offset)),
            buffer.m.planes[i].length };
        if (result[i].data() == MAP_FAILED) {
            throw std::system_error(errno, std::generic_category());
        }
    }
    return result;
}

std::vector<std::string> enumerate_video_devices(udev* ctx, const std::string& media_device)
{
    int fd = errno_wrapper(open, media_device.c_str(), O_RDONLY);

    media_device_info device_info = {};
    errno_wrapper(ioctl, fd, MEDIA_IOC_DEVICE_INFO, &device_info);

    media_v2_topology topology = {};
    errno_wrapper(ioctl, fd, MEDIA_IOC_G_TOPOLOGY, &topology);

    std::vector<media_v2_entity> entities(topology.num_entities);
    std::vector<media_v2_interface> interfaces(topology.num_interfaces);
    topology.ptr_entities = reinterpret_cast<uint64_t>(entities.data());
    topology.ptr_interfaces = reinterpret_cast<uint64_t>(interfaces.data());

    errno_wrapper(ioctl, fd, MEDIA_IOC_G_TOPOLOGY, &topology);
    close(fd);

    // A media device can expose either a decoder or an encoder (or both).  The
    // old decoder-only test made stateful V4L2 encoders invisible to the VA
    // backend, even when their video node advertised M2M_MPLANE.
    if (std::ranges::none_of(entities, [](auto&& entity) {
            return entity.function == MEDIA_ENT_F_PROC_VIDEO_DECODER
                || entity.function == MEDIA_ENT_F_PROC_VIDEO_ENCODER;
        })) {
        return {};
    }

    std::vector<std::string> result;
    for (auto&& interface : interfaces) {
        auto devnum = makedev(interface.devnode.major, interface.devnode.minor);
        std::unique_ptr<udev_device, decltype(&udev_device_unref)> device(
            udev_device_new_from_devnum(ctx, 'c', devnum), &udev_device_unref);
        if (device && interface.intf_type == MEDIA_INTF_T_V4L_VIDEO) {
            result.push_back(udev_device_get_property_value(device.get(), "DEVNAME"));
        }
    }

    return result;
}

std::vector<std::string> enumerate_media_devices(udev* ctx)
{
    std::unique_ptr<udev_enumerate, decltype(&udev_enumerate_unref)> enumerate(
        udev_enumerate_new(ctx), &udev_enumerate_unref);

    udev_enumerate_add_match_subsystem(enumerate.get(), "media");
    udev_enumerate_scan_devices(enumerate.get());

    std::vector<std::string> result;
    for (auto entry = udev_enumerate_get_list_entry(enumerate.get()); entry != nullptr;
         entry = udev_list_entry_get_next(entry)) {
        auto name = udev_list_entry_get_name(entry);

        std::unique_ptr<udev_device, decltype(&udev_device_unref)> device(
            udev_device_new_from_syspath(ctx, name), &udev_device_unref);

        if (device) {
            result.push_back(udev_device_get_property_value(device.get(), "DEVNAME"));
        }
    }

    return result;
}

} // namespace

std::vector<std::pair<std::string, std::optional<std::string>>> V4L2M2MDevice::enumerate_devices()
{
    std::vector<std::pair<std::string, std::optional<std::string>>> result;
    std::set<std::string> seen;
    std::unique_ptr<udev, decltype(&udev_unref)> ctx(udev_new(), &udev_unref);
    if (!ctx) {
        return result;
    }

    const auto add_video = [&](const std::string& path, const std::optional<std::string>& media) {
        if (path.empty() || seen.contains(path)) {
            return;
        }
        // Probe every candidate independently: an inaccessible or non-M2M
        // /dev/video* node must not prevent discovering the encoder.
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        v4l2_capability caps = {};
        const bool valid = ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0;
        close(fd);
        if (!valid) {
            return;
        }
        const uint32_t capabilities = (caps.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? caps.device_caps : caps.capabilities;
        if (!(capabilities & required_capabilities)) {
            return;
        }
        seen.insert(path);
        result.emplace_back(path, media);
    };

    // Preserve Media Controller associations for stateless decoders.  Not all
    // stateful encoders expose a Media Controller topology, however.
    try {
        for (const auto& media_device : enumerate_media_devices(ctx.get())) {
            try {
                for (const auto& video_device : enumerate_video_devices(ctx.get(), media_device)) {
                    add_video(video_device, media_device);
                }
            } catch (const std::exception&) {
                // One broken/inaccessible media device does not hide the rest.
            }
        }
    } catch (const std::exception&) {
        // Continue with video4linux discovery as a fallback.
    }

    std::unique_ptr<udev_enumerate, decltype(&udev_enumerate_unref)> enumerate(
        udev_enumerate_new(ctx.get()), &udev_enumerate_unref);
    if (enumerate) {
        udev_enumerate_add_match_subsystem(enumerate.get(), "video4linux");
        if (udev_enumerate_scan_devices(enumerate.get()) == 0) {
            for (auto entry = udev_enumerate_get_list_entry(enumerate.get()); entry;
                 entry = udev_list_entry_get_next(entry)) {
                std::unique_ptr<udev_device, decltype(&udev_device_unref)> device(
                    udev_device_new_from_syspath(ctx.get(), udev_list_entry_get_name(entry)), &udev_device_unref);
                if (device) {
                    if (const char* devnode = udev_device_get_devnode(device.get())) {
                        add_video(devnode, std::nullopt);
                    }
                }
            }
        }
    }
    return result;
}

V4L2M2MDevice::Buffer::Buffer(V4L2M2MDevice& owner, v4l2_buf_type type, unsigned index)
    : owner_(owner)
    , type_(type)
    , index_(index)
    , mapping_(map_buffer(owner.video_fd, type, index))
{
}

V4L2M2MDevice::Buffer::Buffer(V4L2M2MDevice::Buffer&& other)
    : owner_(other.owner_)
    , type_(other.type_)
    , index_(other.index_)
    , mapping_(other.mapping_)
    , queued_(other.queued_)
    , error_(other.error_)
{
    other.mapping_.clear();
}

V4L2M2MDevice::Buffer& V4L2M2MDevice::Buffer::operator=(V4L2M2MDevice::Buffer&& other)
{
    this->~Buffer();
    new (this) V4L2M2MDevice::Buffer(std::move(other));
    return *this;
}

V4L2M2MDevice::Buffer::~Buffer()
{
    for (auto&& map : mapping_) {
        munmap(map.data(), map.size());
    }
}

void V4L2M2MDevice::Buffer::queue(int request_fd, timeval* timestamp, unsigned size) const
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    struct v4l2_buffer buffer = {
        .index = index_,
        .type = type_,
        .memory = V4L2_MEMORY_MMAP,
        .m = { .planes = planes },
        .length = static_cast<uint32_t>(mapping_.size()),
    };

    if (V4L2_TYPE_IS_MULTIPLANAR(type_)) {
        for (unsigned i = 0; i < mapping_.size(); i++) {
            buffer.m.planes[i].bytesused = size;
        }
    } else {
        buffer.bytesused = size;
    }

    if (request_fd >= 0) {
        buffer.flags = V4L2_BUF_FLAG_REQUEST_FD;
        buffer.request_fd = request_fd;
    }

    if (timestamp != NULL)
        buffer.timestamp = *timestamp;

    errno_wrapper(ioctl, owner_.video_fd, VIDIOC_QBUF, &buffer);
    queued_ = true;
}

void V4L2M2MDevice::Buffer::dequeue() const
{
    // VIDIOC_DQBUF returns whichever buffer was completed first, not a specific one. Keep taking buffers back until
    // this one is among them, and remember the state of the others, so that they aren't dequeued twice later.
    while (queued_) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        struct v4l2_buffer buffer = {
            .index = 0,
            .type = type_,
            .memory = V4L2_MEMORY_MMAP,
            .m = { .planes = planes },
            .length = VIDEO_MAX_PLANES,
        };

        errno_wrapper(ioctl, owner_.video_fd, VIDIOC_DQBUF, &buffer);

        const Buffer& returned = owner_.buffer(type_, buffer.index);
        returned.queued_ = false;
        if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
            returned.error_ = true;
        }
    }

    if (error_) {
        error_ = false;
        throw std::runtime_error("Dequeued buffer marked erroneous by driver.");
    }
}

std::vector<int> V4L2M2MDevice::Buffer::export_(unsigned flags) const
{
    std::vector<int> result;
    for (unsigned i = 0; i < mapping_.size(); i++) {
        v4l2_exportbuffer exportbuffer = {
            .type = type_,
            .index = index_,
            .plane = i,
            .flags = flags,
        };

        errno_wrapper(ioctl, owner_.video_fd, VIDIOC_EXPBUF, &exportbuffer);
        result.push_back(exportbuffer.fd);
    }
    return result;
}

V4L2M2MDevice::V4L2M2MDevice(const std::string& video_path, const std::optional<std::string>& media_path)
    : video_path(video_path)
    , media_path(media_path)
    , video_fd(errno_wrapper(open, video_path.c_str(), O_RDWR | O_NONBLOCK))
    , media_fd((media_path) ? errno_wrapper(open, media_path->c_str(), O_RDWR | O_NONBLOCK) : -1)
    , capabilities(query_capabilities(video_fd))
    , capture_buf_type(
          (capabilities & V4L2_CAP_VIDEO_M2M) ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
    , output_buf_type(
          (capabilities & V4L2_CAP_VIDEO_M2M) ? V4L2_BUF_TYPE_VIDEO_OUTPUT : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
    , capture_format(get_format(video_fd, capture_buf_type))
    , output_format(get_format(video_fd, output_buf_type))
{
    if (!(capabilities & required_capabilities)) {
        std::runtime_error("Missing device capabilities");
    }
}

V4L2M2MDevice::V4L2M2MDevice(V4L2M2MDevice&& other)
    : video_path(std::move(other.video_path))
    , media_path(std::move(other.media_path))
    , video_fd(std::move(other.video_fd))
    , media_fd(std::move(other.media_fd))
    , capabilities(std::move(other.capabilities))
    , capture_buf_type(std::move(other.capture_buf_type))
    , output_buf_type(std::move(other.output_buf_type))
    , capture_format(std::move(other.capture_format))
    , output_format(std::move(other.output_format))
    , supported_output_formats(std::move(other.supported_output_formats))
    , supported_capture_formats(std::move(other.supported_capture_formats))
    , capture_buffers(std::move(other.capture_buffers))
    , output_buffers(std::move(other.output_buffers))
{
    other.capture_buffers.clear();
    other.output_buffers.clear();
    other.video_fd = -1;
    other.media_fd = -1;
}

V4L2M2MDevice& V4L2M2MDevice::operator=(V4L2M2MDevice&& other)
{
    this->~V4L2M2MDevice();
    new (this) V4L2M2MDevice(std::move(other));
    return *this;
}

V4L2M2MDevice::~V4L2M2MDevice()
{
    if (video_fd >= 0) {
        close(video_fd);
    }
    if (media_fd >= 0) {
        close(media_fd);
    }
}

void V4L2M2MDevice::set_format(v4l2_buf_type type, unsigned int pixelformat, unsigned int width, unsigned int height)
{
    struct v4l2_format* format = V4L2_TYPE_IS_CAPTURE(type) ? &capture_format : &output_format;

    format->type = type;
    format->fmt.pix_mp.pixelformat = pixelformat;
    format->fmt.pix_mp.width = width;
    format->fmt.pix_mp.height = height;

    // Automatic size is insufficient for data buffers. A single frame of a high bitrate stream can be a few MB, so
    // scale with the picture size (that's what the MediaTek driver picks on its own, too).
    format->fmt.pix_mp.plane_fmt[0].sizeimage
        = V4L2_TYPE_IS_OUTPUT(type) ? std::max<unsigned>(SOURCE_SIZE_MAX, width * height * 3 / 2) : 0;

    errno_wrapper(ioctl, video_fd, VIDIOC_S_FMT, format);
}

namespace {

const char* control_name(uint32_t id)
{
    switch (id) {
    case V4L2_CID_STATELESS_H264_SPS: return "H264_SPS";
    case V4L2_CID_STATELESS_H264_PPS: return "H264_PPS";
    case V4L2_CID_STATELESS_H264_SCALING_MATRIX: return "H264_SCALING_MATRIX";
    case V4L2_CID_STATELESS_H264_PRED_WEIGHTS: return "H264_PRED_WEIGHTS";
    case V4L2_CID_STATELESS_H264_SLICE_PARAMS: return "H264_SLICE_PARAMS";
    case V4L2_CID_STATELESS_H264_DECODE_PARAMS: return "H264_DECODE_PARAMS";
    case V4L2_CID_STATELESS_H264_DECODE_MODE: return "H264_DECODE_MODE";
    case V4L2_CID_STATELESS_H264_START_CODE: return "H264_START_CODE";
    default: return "unknown control";
    }
}

} // namespace

std::string V4L2M2MDevice::describe_format(v4l2_buf_type type) const
{
    v4l2_format format = {};
    format.type = type;
    if (ioctl(video_fd, VIDIOC_G_FMT, &format) < 0) {
        return "VIDIOC_G_FMT failed";
    }

    char text[512];
    if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
        const auto& pix = format.fmt.pix_mp;
        int n = snprintf(text, sizeof(text), "%ux%u '%.4s' %u planes:", pix.width, pix.height,
            reinterpret_cast<const char*>(&pix.pixelformat), pix.num_planes);
        for (unsigned i = 0; i < pix.num_planes && i < VIDEO_MAX_PLANES && n > 0 && n < (int)sizeof(text); i++) {
            n += snprintf(text + n, sizeof(text) - n, " [bytesperline %u, sizeimage %u]", pix.plane_fmt[i].bytesperline,
                pix.plane_fmt[i].sizeimage);
        }
    } else {
        const auto& pix = format.fmt.pix;
        snprintf(text, sizeof(text), "%ux%u '%.4s' [bytesperline %u, sizeimage %u]", pix.width, pix.height,
            reinterpret_cast<const char*>(&pix.pixelformat), pix.bytesperline, pix.sizeimage);
    }
    return text;
}

std::string V4L2M2MDevice::refresh_capture_format()
{
    capture_format = get_format(video_fd, capture_buf_type);
    return describe_format(capture_buf_type);
}

unsigned V4L2M2MDevice::request_buffers(v4l2_buf_type type, unsigned count, bool cpu_cached)
{
    struct v4l2_requestbuffers req_buffers = {
        .count = count,
        .type = type,
        .memory = V4L2_MEMORY_MMAP,
        .flags = static_cast<__u8>(cpu_cached ? V4L2_MEMORY_FLAG_NON_COHERENT : 0),
    };

    errno_wrapper(ioctl, video_fd, VIDIOC_REQBUFS, &req_buffers);

    std::lock_guard<std::mutex> guard(*buffers_mutex);
    auto& buffers = V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers;

    buffers.clear();
    for (unsigned i = 0; i < req_buffers.count; i += 1) {
        buffers.emplace_back(*this, type, i);
    }

    return buffers.size(); // Actual amount may differ
}

bool V4L2M2MDevice::format_supported(v4l2_buf_type type, unsigned pixelformat) const
{
    for (v4l2_fmtdesc fmtdesc = { .type = type }; ioctl(video_fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0; fmtdesc.index += 1) {
        if (fmtdesc.pixelformat == pixelformat) {
            return true;
        }
    }
    return false;
}

const V4L2M2MDevice::Buffer& V4L2M2MDevice::buffer(v4l2_buf_type type, unsigned index)
{
    std::lock_guard<std::mutex> guard(*buffers_mutex);
    return (V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers).at(index);
}

std::optional<V4L2M2MDevice::FrameSizeLimits> V4L2M2MDevice::frame_size_limits(unsigned pixelformat) const
{
    FrameSizeLimits limits;
    bool found_any = false;

    const auto include_bounds = [&](unsigned min_width, unsigned min_height, unsigned max_width, unsigned max_height) {
        if (!found_any) {
            limits.min_width = min_width;
            limits.min_height = min_height;
            limits.max_width = max_width;
            limits.max_height = max_height;
            found_any = true;
            return;
        }
        limits.min_width = std::min(limits.min_width, min_width);
        limits.min_height = std::min(limits.min_height, min_height);
        limits.max_width = std::max(limits.max_width, max_width);
        limits.max_height = std::max(limits.max_height, max_height);
    };

    for (unsigned index = 0;; index++) {
        struct v4l2_frmsizeenum size = {};
        size.index = index;
        size.pixel_format = pixelformat;
        if (ioctl(video_fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) {
            // EINVAL is the V4L2 end-of-enumeration / unsupported-format indication. Other failures mean that
            // optional size enumeration is unavailable; do not let a capability query break the VA-API entrypoint.
            if (errno == EINVAL) {
                break;
            }
            fprintf(stderr, "libva-v4l2: VIDIOC_ENUM_FRAMESIZES failed: %s\n", strerror(errno));
            return std::nullopt;
        }

        switch (size.type) {
        case V4L2_FRMSIZE_TYPE_DISCRETE: {
            const unsigned width = size.discrete.width;
            const unsigned height = size.discrete.height;
            limits.discrete_sizes.emplace_back(width, height);
            include_bounds(width, height, width, height);
            break;
        }
        case V4L2_FRMSIZE_TYPE_CONTINUOUS:
        case V4L2_FRMSIZE_TYPE_STEPWISE: {
            const auto& stepwise = size.stepwise;
            const unsigned step_width = size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS
                ? 1 : std::max(1u, stepwise.step_width);
            const unsigned step_height = size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS
                ? 1 : std::max(1u, stepwise.step_height);
            if (stepwise.min_width == 0 || stepwise.min_height == 0 || stepwise.min_width > stepwise.max_width
                || stepwise.min_height > stepwise.max_height) {
                fprintf(stderr, "libva-v4l2: VIDIOC_ENUM_FRAMESIZES returned an invalid range\n");
                return std::nullopt;
            }
            limits.ranges.push_back({ stepwise.min_width, stepwise.min_height, stepwise.max_width,
                stepwise.max_height, step_width, step_height });
            include_bounds(stepwise.min_width, stepwise.min_height, stepwise.max_width, stepwise.max_height);
            break;
        }
        default:
            fprintf(stderr, "libva-v4l2: VIDIOC_ENUM_FRAMESIZES returned an unknown frame-size type\n");
            return std::nullopt;
        }
    }

    if (!found_any) {
        return std::nullopt;
    }
    return limits;
}

unsigned V4L2M2MDevice::buffer_count(v4l2_buf_type type) const
{
    std::lock_guard<std::mutex> guard(*buffers_mutex);
    return (V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers).size();
}

unsigned V4L2M2MDevice::create_buffers(v4l2_buf_type type, unsigned count, bool cpu_cached)
{
    struct v4l2_create_buffers create = {};
    create.count = count;
    create.memory = V4L2_MEMORY_MMAP;
    create.format = get_format(video_fd, type);
    create.flags = cpu_cached ? V4L2_MEMORY_FLAG_NON_COHERENT : 0;

    errno_wrapper(ioctl, video_fd, VIDIOC_CREATE_BUFS, &create);

    std::lock_guard<std::mutex> guard(*buffers_mutex);
    auto& buffers = V4L2_TYPE_IS_CAPTURE(type) ? capture_buffers : output_buffers;
    if (create.index != buffers.size()) {
        throw std::runtime_error("The driver numbered new buffers unexpectedly");
    }
    for (unsigned i = 0; i < create.count; i += 1) {
        buffers.emplace_back(*this, type, create.index + i);
    }

    return create.count;
}

int32_t V4L2M2MDevice::get_control(uint32_t id) const {
    v4l2_control ctrl = {
        .id = id,
    };
    errno_wrapper(ioctl, video_fd, VIDIOC_G_CTRL, &ctrl);
    return ctrl.value;
}

void V4L2M2MDevice::set_ext_control(int request_fd, unsigned id, void* data, unsigned size)
{
    v4l2_ext_control control = {
        .id = id,
        .size = size,
        .ptr = data,
    };
    set_ext_controls(request_fd, std::span(&control, 1));
}

void V4L2M2MDevice::set_ext_controls(int request_fd, std::span<v4l2_ext_control> controls)
{
    struct v4l2_ext_controls meta = {
        .count = static_cast<uint32_t>(controls.size()),
        .controls = controls.data(),
    };

    if (request_fd >= 0) {
        meta.which = V4L2_CTRL_WHICH_REQUEST_VAL;
        meta.request_fd = request_fd;
    }

    if (ioctl(video_fd, VIDIOC_S_EXT_CTRLS, &meta) < 0) {
        const int error = errno;
        // error_idx points at the rejected control, or equals the count if the set as a whole was refused.
        std::string what = "VIDIOC_S_EXT_CTRLS";
        if (meta.error_idx < controls.size()) {
            what += std::string(" rejected ") + control_name(controls[meta.error_idx].id);
        } else {
            what += " rejected the control set as a whole";
        }
        throw std::system_error(error, std::generic_category(), what);
    }
}

void V4L2M2MDevice::set_streaming(bool enable)
{
    errno_wrapper(ioctl, video_fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &capture_buf_type);
    errno_wrapper(ioctl, video_fd, enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &output_buf_type);
}
