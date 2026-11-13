#include "h264_encode.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <poll.h>
#include <stdexcept>
#include <new>
#include <utility>
#include <system_error>

extern "C" {
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va.h>
}

#include "buffer.h"
#include "dmabuf.h"
#include "drm.h"
#include "driver.h"
#include "format.h"
#include "linux/v4l2-controls.h"
#include "surface.h"
#include "encoder_debug_flag.h"
#include "utils.h"

namespace {

constexpr unsigned kQueueBuffers = 4;
constexpr unsigned kDefaultBitrate = 4'000'000;
constexpr unsigned kCaptureSizeFloor = 4 * 1024 * 1024;
constexpr int kPollTimeoutMs = 10'000;

// DRM dumb buffers are mmap'ed by the DRM fd. Synchronize CPU writes with
// devices importing their PRIME fd, also on non-cache-coherent ARM platforms.
struct DmaBufCpuWriteGuard {
    std::vector<int> fds;
    explicit DmaBufCpuWriteGuard(const std::vector<std::unique_ptr<DrmBuffer>>& buffers)
    {
        fds.reserve(buffers.size());
        try {
            for (const auto& buffer : buffers) {
                const int fd = buffer->duplicate_fd();
                dma_buf_sync sync = {};
                sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
                if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
                    const int error = errno;
                    close(fd);
                    throw std::system_error(error, std::generic_category(), "starting DRM DMA-BUF CPU write");
                }
                fds.push_back(fd);
            }
        } catch (...) {
            const int cleanup_error = end_impl();
            if (cleanup_error)
                std::fprintf(stderr, "libva-v4l2: DMA-BUF CPU write END during failed START: %s\n", std::strerror(cleanup_error));
            throw;
        }
    }
    DmaBufCpuWriteGuard(const DmaBufCpuWriteGuard&) = delete;
    DmaBufCpuWriteGuard& operator=(const DmaBufCpuWriteGuard&) = delete;
    // Complete the CPU write before the device can import the planes.
    // Close all temporary fds even if one DMA-BUF END fails.
    void end()
    {
        const int error = end_impl();
        if (error)
            throw std::system_error(error, std::generic_category(), "ending DRM DMA-BUF CPU write");
    }
    ~DmaBufCpuWriteGuard() noexcept
    {
        const int error = end_impl();
        if (error)
            std::fprintf(stderr, "libva-v4l2: DMA-BUF CPU write END during cleanup failed: %s\n", std::strerror(error));
    }
private:
    int end_impl() noexcept
    {
        int first_error = 0;
        for (int fd : fds) {
            dma_buf_sync sync = {};
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
            if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && !first_error)
                first_error = errno;
            close(fd);
        }
        fds.clear();
        return first_error;
    }
};

// Preserve the old full-plane initialization semantics while avoiding writes
// to the rectangle immediately overwritten by MM21 detiling.  In particular,
// clear right-edge stride padding, bottom coded rows, and any trailing allocation
// slack.  Do not touch the active pixel region.
void fill_nv12m_padding(std::span<uint8_t> plane, unsigned pitch, unsigned active_width,
    unsigned active_rows, uint8_t neutral)
{
    if (!pitch || pitch < active_width || !active_rows
        || active_rows > plane.size() / pitch
        || (plane.size() - static_cast<size_t>(active_rows - 1) * pitch) < active_width)
        throw std::runtime_error("invalid NV12M padding geometry");
    for (unsigned row = 0; row < active_rows; ++row) {
        const size_t off = static_cast<size_t>(row) * pitch;
        const size_t end = std::min(plane.size(), off + pitch);
        std::fill(plane.begin() + off + active_width, plane.begin() + end, neutral);
    }
    const size_t after_active = std::min(plane.size(), static_cast<size_t>(active_rows) * pitch);
    std::fill(plane.begin() + after_active, plane.end(), neutral);
}

// CLOCK_THREAD_CPUTIME_ID excludes time spent off-CPU; steady_clock measures
// the elapsed wall time. These timings deliberately use the same thread.
uint64_t thread_cpu_ns()
{
    timespec ts = {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        throw std::system_error(errno, std::generic_category(), "reading thread CPU clock");
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

uint64_t wall_ns()
{
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

void ioctl_or_throw(int fd, unsigned long request, void* argument, const char* operation)
{
    if (ioctl(fd, request, argument) < 0) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
}

unsigned va_profile_to_v4l2(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264High:
        return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
    case VAProfileH264Main:
        return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    case VAProfileH264ConstrainedBaseline:
        // MT8183 exposes Baseline (not Constrained Baseline) through V4L2.
        return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
    default:
        return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
    }
}

unsigned level_idc_to_v4l2(uint8_t level_idc)
{
    switch (level_idc) {
    case 9: return V4L2_MPEG_VIDEO_H264_LEVEL_1B;
    case 10: return V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
    case 11: return V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
    case 12: return V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
    case 13: return V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
    case 20: return V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
    case 21: return V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
    case 22: return V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
    case 30: return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
    case 31: return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
    case 32: return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
    case 41: return V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
    case 42: return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
    case 50: return V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
    case 51: return V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
    case 40:
    default:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
    }
}

unsigned minimum_level_for_frame_size(int width, int height)
{
    // H.264 MaxFS limits. The VENC driver aligns the raw height to 32 lines
    // (720 therefore becomes 736), and that aligned height is also the size
    // reported by the encoded bitstream on this kernel.
    const unsigned macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
    if (macroblocks > 8192) {
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
    }
    if (macroblocks > 5120) {
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
    }
    if (macroblocks > 3600) {
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
    }
    if (macroblocks > 1620) {
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
    }
    return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
}

unsigned pick_default_level(int width, int height)
{
    // Match the VENC driver's default (Level 4.0), while never selecting a
    // level below the frame-size requirement.
    return std::max<unsigned>(V4L2_MPEG_VIDEO_H264_LEVEL_4_0,
        minimum_level_for_frame_size(width, height));
}

} // namespace

H264EncoderContext::MappedBuffer::MappedBuffer(MappedBuffer&& other) noexcept
    : planes(std::move(other.planes))
    , dma_planes(std::move(other.dma_planes))
    , queued(std::exchange(other.queued, false))
{
    other.planes.clear();
}

H264EncoderContext::MappedBuffer& H264EncoderContext::MappedBuffer::operator=(MappedBuffer&& other) noexcept
{
    if (this != &other) {
        this->~MappedBuffer();
        new (this) MappedBuffer(std::move(other));
    }
    return *this;
}

H264EncoderContext::MappedBuffer::~MappedBuffer()
{
    if (dma_planes.empty()) {
        for (const auto& plane : planes) {
            if (plane.data() && !plane.empty()) {
                munmap(plane.data(), plane.size());
            }
        }
    }
}

std::set<VAProfile> H264EncoderContext::supported_profiles(const V4L2M2MDevice& device)
{
    if (!supports_nv12_surfaces(device)) {
        return {};
    }
    return { VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High };
}

bool H264EncoderContext::supports_nv12_surfaces(const V4L2M2MDevice& device)
{
    return device.format_supported(device.output_buf_type, V4L2_PIX_FMT_NV12M)
        && device.format_supported(device.capture_buf_type, V4L2_PIX_FMT_H264);
}

H264EncoderContext::H264EncoderContext(DriverData* driver_data, V4L2M2MDevice& device, VAProfile profile,
    int picture_width, int picture_height)
    : Context(driver_data, device, V4L2_PIX_FMT_NV12M, picture_width, picture_height, {})
    , va_profile(profile)
    , visible_width(static_cast<unsigned>(picture_width))
    , visible_height(static_cast<unsigned>(picture_height))
{
    encoding = true;
    if ((picture_width & 1) || (picture_height & 1)) {
        throw std::invalid_argument("H.264 NV12 encoding requires even surface dimensions");
    }

    if (device.output_format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M
        || device.output_format.fmt.pix_mp.num_planes != 2) {
        throw std::runtime_error("the MT8183 encoder did not negotiate two-plane NV12M input");
    }

    // VIDIOC_S_FMT returns the padded buffer dimensions (e.g. 1280x736).
    // The H.264 bitstream must describe the requested visible dimensions
    // separately.  The kernel's S_SELECTION handler must accept OUTPUT_MPLANE
    // for this to work (older MT8183 drivers accidentally check VIDEO_OUTPUT).
    if (device.output_format.fmt.pix_mp.width != static_cast<unsigned>(picture_width)
        || device.output_format.fmt.pix_mp.height != static_cast<unsigned>(picture_height)) {
        v4l2_selection crop = {};
        crop.type = device.output_buf_type;
        crop.target = V4L2_SEL_TGT_CROP;
        crop.r.left = 0;
        crop.r.top = 0;
        crop.r.width = visible_width;
        crop.r.height = visible_height;
        if (ioctl(device.video_fd, VIDIOC_S_SELECTION, &crop) < 0) {
            // Leave the pre-existing encoder path functional on old kernels.
            std::fprintf(stderr,
                "libva-v4l2: VENC visible crop %dx%d could not be set: %s; "
                "H.264 stream may report padded dimensions\n",
                picture_width, picture_height, std::strerror(errno));
        } else if (crop.r.width != visible_width || crop.r.height != visible_height) {
            throw std::runtime_error("MT8183 encoder did not accept the requested visible crop");
        }
    }

    set_capture_format();
    // Opt-in staging via two DRM PRIME planes. This validates V4L2 DMA-BUF
    // import; CPU copy into these planes is intentionally retained for now.
    const char* dma_env = std::getenv("LIBVA_V4L2_ENC_DMABUF");
    if (dma_env && std::strcmp(dma_env, "1") == 0) {
        try {
            allocate_dmabuf_input();
            use_dmabuf_input = true;
            std::fprintf(stderr, "libva-v4l2: VENC DMA-BUF input enabled (staging default; direct PRIME optional)\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "libva-v4l2: VENC DMA-BUF setup failed: %s; falling back to MMAP\n", e.what());
            input_buffers.clear();
            v4l2_requestbuffers drop = {};
            drop.type = device.output_buf_type;
            drop.memory = V4L2_MEMORY_DMABUF;
            ioctl(device.video_fd, VIDIOC_REQBUFS, &drop);
        }
    }
    // If either MMAP queue setup fails after REQBUFS, the constructor throws
    // and ~H264EncoderContext() will not run. Release any partially allocated
    // V4L2 queues here while the members are still alive.
    try {
        if (!use_dmabuf_input) {
            allocate_queue(device.output_buf_type, input_buffers, kQueueBuffers);
        }
        allocate_queue(device.capture_buf_type, capture_buffers, kQueueBuffers);
    } catch (...) {
        release_queues();
        throw;
    }

    // Allocate CPU-mappable linear NV12 backing for encode-only surfaces.  A
    // surface produced by the decoder or imported from PRIME keeps its own
    // backing and layout instead.
}

H264EncoderContext::~H264EncoderContext()
{
    if (encoder_debug_enabled()) {
        std::fprintf(stderr,
            "libva-v4l2: VENC input statistics: frames=%llu direct_prime=%llu dmabuf_staging=%llu mmap=%llu layout_fallbacks=%llu import_rejections=%llu\n",
            static_cast<unsigned long long>(frame_number),
            static_cast<unsigned long long>(direct_prime_qbuf),
            static_cast<unsigned long long>(dmabuf_staging_qbuf),
            static_cast<unsigned long long>(mmap_qbuf),
            static_cast<unsigned long long>(direct_layout_fallbacks),
            static_cast<unsigned long long>(direct_import_rejections));
    if (direct_convert_frames || direct_convert_fallbacks)
        std::fprintf(stderr, "libva-v4l2: TRANSCODE DIRECT CONVERT stats: frames=%llu fallbacks=%llu\n",
            direct_convert_frames, direct_convert_fallbacks);
    if (direct_convert_frames) {
        std::fprintf(stderr,
            "libva-v4l2: TRANSCODE DIRECT CONVERT timing: frames=%llu padding_only=%llu padding_ms=%.3f detile_ms=%.3f total_ms=%.3f (CPU wall time; includes DMA-BUF sync in total)\n",
            direct_convert_frames, direct_convert_padding_only_frames,
            direct_convert_padding_ns / 1000000.0, direct_convert_detile_ns / 1000000.0,
            direct_convert_total_ns / 1000000.0);
    }
    if (benchmark_direct_frames || benchmark_ramcopy_frames) {
        std::fprintf(stderr,
            "libva-v4l2: TRANSCODE MEMORY BENCH frames_direct=%llu frames_ramcopy=%llu "
            "direct_wall_ms=%.3f direct_cpu_ms=%.3f dma_detile_cpu_ms=%.3f "
            "ram_detile_wall_ms=%.3f ram_detile_cpu_ms=%.3f "
            "dma_copy_wall_ms=%.3f dma_copy_cpu_ms=%.3f "
            "ramcopy_wall_ms=%.3f ramcopy_cpu_ms=%.3f\n",
            static_cast<unsigned long long>(benchmark_direct_frames),
            static_cast<unsigned long long>(benchmark_ramcopy_frames),
            benchmark_direct_wall_ns / 1e6, benchmark_direct_cpu_ns / 1e6,
            benchmark_dma_detile_cpu_ns / 1e6,
            benchmark_ram_detile_wall_ns / 1e6, benchmark_ram_detile_cpu_ns / 1e6,
            benchmark_dma_copy_wall_ns / 1e6, benchmark_dma_copy_cpu_ns / 1e6,
            benchmark_ramcopy_wall_ns / 1e6, benchmark_ramcopy_cpu_ns / 1e6);
    }
    }
    release_queues();
}

void H264EncoderContext::set_capture_format()
{
    v4l2_format format = {};
    format.type = device.capture_buf_type;
    format.fmt.pix_mp.width = device.output_format.fmt.pix_mp.width;
    format.fmt.pix_mp.height = device.output_format.fmt.pix_mp.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = std::max<unsigned>(kCaptureSizeFloor,
        format.fmt.pix_mp.width * format.fmt.pix_mp.height * 2);
    ioctl_or_throw(device.video_fd, VIDIOC_S_FMT, &format, "setting H.264 CAPTURE format");
    if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264 || format.fmt.pix_mp.num_planes != 1) {
        throw std::runtime_error("the MT8183 encoder did not negotiate one-plane H.264 output");
    }
    device.capture_format = format;
}

void H264EncoderContext::allocate_queue(v4l2_buf_type type, std::vector<MappedBuffer>& buffers, unsigned count)
{
    v4l2_requestbuffers request = {};
    request.count = count;
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;
    ioctl_or_throw(device.video_fd, VIDIOC_REQBUFS, &request, "requesting V4L2 encoder buffers");
    if (request.count < 2) {
        throw std::runtime_error("V4L2 encoder returned fewer than two MMAP buffers");
    }

    buffers.reserve(request.count);
    for (unsigned index = 0; index < request.count; index++) {
        std::array<v4l2_plane, VIDEO_MAX_PLANES> planes = {};
        v4l2_buffer query = {};
        query.type = type;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        query.m.planes = planes.data();
        query.length = VIDEO_MAX_PLANES;
        ioctl_or_throw(device.video_fd, VIDIOC_QUERYBUF, &query, "querying V4L2 encoder buffer");

        MappedBuffer mapped;
        mapped.planes.reserve(query.length);
        for (unsigned plane = 0; plane < query.length; plane++) {
            void* address = mmap(nullptr, planes[plane].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                device.video_fd, planes[plane].m.mem_offset);
            if (address == MAP_FAILED) {
                throw std::system_error(errno, std::generic_category(), "mapping V4L2 encoder buffer");
            }
            mapped.planes.emplace_back(static_cast<uint8_t*>(address), planes[plane].length);
        }
        buffers.emplace_back(std::move(mapped));
    }
}

void H264EncoderContext::allocate_dmabuf_input()
{
    v4l2_requestbuffers request = {};
    request.type = device.output_buf_type;
    request.memory = V4L2_MEMORY_DMABUF;
    request.count = kQueueBuffers;
    ioctl_or_throw(device.video_fd, VIDIOC_REQBUFS, &request, "requesting V4L2 DMA-BUF OUTPUT slots");
    if (request.count < 2) {
        throw std::runtime_error("encoder returned fewer than two DMA-BUF slots");
    }
    input_buffers.reserve(request.count);
    const auto& fmt = device.output_format.fmt.pix_mp;
    for (unsigned index = 0; index < request.count; ++index) {
        MappedBuffer mapped;
        mapped.dma_planes.reserve(2);
        mapped.planes.reserve(2);
        for (unsigned plane = 0; plane < 2; ++plane) {
            auto drm = std::make_unique<DrmBuffer>(DrmBuffer::allocate(fmt.plane_fmt[plane].sizeimage));
            if (drm->size() < fmt.plane_fmt[plane].sizeimage) {
                throw std::runtime_error("DRM allocation smaller than V4L2 plane");
            }
            mapped.planes.emplace_back(drm->data(), drm->size());
            mapped.dma_planes.emplace_back(std::move(drm));
        }
        input_buffers.emplace_back(std::move(mapped));
    }
}

void H264EncoderContext::switch_input_to_mmap()
{
    if (streaming) {
        throw std::runtime_error("cannot switch encoder OUTPUT memory after STREAMON");
    }
    std::fprintf(stderr, "libva-v4l2: VENC DMA-BUF QBUF rejected; using MMAP fallback\n");
    input_buffers.clear();
    v4l2_requestbuffers drop = {};
    drop.type = device.output_buf_type;
    drop.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(device.video_fd, VIDIOC_REQBUFS, &drop) < 0) {
        throw std::system_error(errno, std::generic_category(), "releasing DMA-BUF OUTPUT queue");
    }
    use_dmabuf_input = false;
    allocate_queue(device.output_buf_type, input_buffers, kQueueBuffers);
}

void H264EncoderContext::stop_streaming() noexcept
{
    if (device.video_fd >= 0 && streaming) {
        for (const auto type : { device.capture_buf_type, device.output_buf_type }) {
            auto mutable_type = type;
            ioctl(device.video_fd, VIDIOC_STREAMOFF, &mutable_type);
        }
    }
    streaming = false;
    for (auto& buffer : input_buffers) {
        buffer.queued = false;
    }
    for (auto& buffer : capture_buffers) {
        buffer.queued = false;
    }
}

void H264EncoderContext::release_queues() noexcept
{
    if (device.video_fd < 0) {
        return;
    }
    stop_streaming();

    input_buffers.clear();
    capture_buffers.clear();
    for (const auto type : { device.capture_buf_type, device.output_buf_type }) {
        v4l2_requestbuffers request = {};
        request.count = 0;
        request.type = type;
        request.memory = type == device.output_buf_type && use_dmabuf_input
            ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        ioctl(device.video_fd, VIDIOC_REQBUFS, &request);
    }
}

int H264EncoderContext::set_control(uint32_t id, int32_t value) const
{
    v4l2_control control = {};
    control.id = id;
    control.value = value;
    if (ioctl(device.video_fd, VIDIOC_S_CTRL, &control) < 0) {
        return -errno;
    }
    return 0;
}

unsigned H264EncoderContext::h264_profile_control() const
{
    return va_profile_to_v4l2(va_profile);
}

unsigned H264EncoderContext::h264_level_control() const
{
    const unsigned width = device.output_format.fmt.pix_mp.width;
    const unsigned height = device.output_format.fmt.pix_mp.height;
    const unsigned minimum = minimum_level_for_frame_size(width, height);
    if (sequence && sequence->level_idc) {
        return std::max(level_idc_to_v4l2(sequence->level_idc), minimum);
    }
    return pick_default_level(width, height);
}

int H264EncoderContext::set_controls()
{
    if (controls_applied && !controls_dirty) {
        return 0;
    }

    const auto apply = [&](uint32_t id, int32_t value, bool required = true) {
        const int ret = set_control(id, value);
        if (ret < 0) {
            fprintf(stderr, "libva-v4l2: encoder control 0x%x=%d failed: %s%s\n", id, value,
                strerror(-ret), required ? " (required)" : " (ignored)");
        }
        return ret >= 0 || !required;
    };

    // MT8183 firmware rate control needs an explicitly configured frame rate.
    // The kernel default can otherwise yield zero FPS after integer division.
    // The ioctl uses the multi-planar OUTPUT queue type.
    if (!framerate_applied || framerate_dirty) {
        v4l2_streamparm parm = {};
        parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        parm.parm.output.timeperframe.numerator = fps_denominator;
        parm.parm.output.timeperframe.denominator = fps_numerator;
        if (ioctl(device.video_fd, VIDIOC_S_PARM, &parm) < 0) {
            fprintf(stderr, "libva-v4l2: encoder S_PARM %u/%u FPS failed: %s\n",
                fps_numerator, fps_denominator, strerror(errno));
            return -1;
        }
        if (encoder_debug_enabled()) {
            fprintf(stderr, "libva-v4l2: encoder S_PARM requested FPS=%u/%u, returned timeperframe=%u/%u\n",
                fps_numerator, fps_denominator,
                parm.parm.output.timeperframe.numerator,
                parm.parm.output.timeperframe.denominator);
        }
        framerate_applied = true;
        framerate_dirty = false;
    }

    const unsigned bitrate = target_bitrate.value_or(
        sequence && sequence->bits_per_second ? sequence->bits_per_second : kDefaultBitrate);
    bool ok = true;
    if (!controls_applied) {
        const unsigned gop = sequence ? sequence->intra_period : 0;
        const unsigned i_period = sequence ? sequence->intra_idr_period : 0;
        ok &= apply(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
        ok &= apply(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
        ok &= apply(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1);
        ok &= apply(V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
        ok &= apply(V4L2_CID_MPEG_VIDEO_H264_PROFILE, h264_profile_control());
        ok &= apply(V4L2_CID_MPEG_VIDEO_H264_LEVEL, h264_level_control());
        if (gop) {
            ok &= apply(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, false);
        }
        if (i_period) {
            ok &= apply(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, i_period, false);
        }
    }

    // Rate-control parameters may be sent again between pictures.  Apply the
    // bitrate update on the next vaEndPicture instead of silently retaining
    // the first sequence's value.
    ok &= apply(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
    // Read-back verifies the V4L2 control value, not that firmware actually
    // achieves the requested rate.  Enable with LIBVA_V4L2_ENC_DEBUG=1.
    if (encoder_debug_enabled()) {
        v4l2_control actual = {};
        actual.id = V4L2_CID_MPEG_VIDEO_BITRATE;
        const int ret = ioctl(device.video_fd, VIDIOC_G_CTRL, &actual);
        fprintf(stderr, "libva-v4l2: H264 requested bitrate=%u bps, V4L2 readback=%s%d, streaming=%d\n",
            bitrate, ret == 0 ? "" : "ERR:", ret == 0 ? actual.value : errno, streaming);
    }
    if (maximum_qp && *maximum_qp > 0 && *maximum_qp <= 51) {
        ok &= apply(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, *maximum_qp, false);
    }
    if (!ok) {
        return -1;
    }
    controls_applied = true;
    controls_dirty = false;
    return 0;
}

VAStatus H264EncoderContext::prepare_surface(VASurfaceID surface_id)
{
    auto found = driver_data->surfaces.find(surface_id);
    if (found == driver_data->surfaces.end()) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = found->second;
    // Read-only compatibility probe. No imported buffer is queued or retained:
    // V4L2 CAPTURE ownership and decoder/encoder synchronization must be
    // proven separately before enabling a real zero-copy path.
    if (!transcode_probe_logged && std::getenv("LIBVA_V4L2_TRANSCODE_PROBE")) {
        transcode_probe_logged = true;
        const auto& enc = device.output_format.fmt.pix_mp;
        std::fprintf(stderr,
            "libva-v4l2: TRANSCODE PROBE source surface=%u %ux%u decoder_capture=%d conversion=%u "
            "converted=%d converted_exportable=%d imported=%d drm_encoder_planes=%zu pending_sync=%d queued=%d\n",
            surface_id, surface.width, surface.height, surface.destination_buffer.has_value(),
            static_cast<unsigned>(surface.conversion), static_cast<bool>(surface.converted_buffer),
            surface.converted_buffer && surface.converted_buffer->exportable(),
            static_cast<bool>(surface.imported_buffer), surface.encoder_drm_planes.size(),
            surface.pending_sync, surface.queued);
        std::fprintf(stderr,
            "libva-v4l2: TRANSCODE PROBE VENC OUTPUT fourcc=%c%c%c%c planes=%u size=%ux%u "
            "Y(stride=%u bytes=%u) UV(stride=%u bytes=%u)\n",
            enc.pixelformat & 255, (enc.pixelformat >> 8) & 255,
            (enc.pixelformat >> 16) & 255, (enc.pixelformat >> 24) & 255,
            enc.num_planes, enc.width, enc.height,
            enc.plane_fmt[0].bytesperline, enc.plane_fmt[0].sizeimage,
            enc.plane_fmt[1].bytesperline, enc.plane_fmt[1].sizeimage);
        const auto& layout = surface.logical_destination_layout;
        for (unsigned i = 0; i < layout.size(); ++i) {
            std::fprintf(stderr,
                "libva-v4l2: TRANSCODE PROBE logical[%u] physical=%u pitch=%u offset=%u bytes=%u\n",
                i, layout[i].physical_plane_index, layout[i].pitch,
                layout[i].offset, layout[i].size);
        }
        if (surface.destination_buffer) {
            const auto& cap = surface.destination_buffer->get();
            const auto& fmt = cap.owner().capture_format.fmt.pix_mp;
            const auto maps = cap.mapping();
            std::fprintf(stderr,
                "libva-v4l2: TRANSCODE PROBE VDEC CAPTURE fourcc=%c%c%c%c planes=%u size=%ux%u\n",
                fmt.pixelformat & 255, (fmt.pixelformat >> 8) & 255,
                (fmt.pixelformat >> 16) & 255, (fmt.pixelformat >> 24) & 255,
                fmt.num_planes, fmt.width, fmt.height);
            for (unsigned i = 0; i < fmt.num_planes; ++i) {
                std::fprintf(stderr,
                    "libva-v4l2: TRANSCODE PROBE capture[%u] stride=%u bytes=%u mapped=%zu\n",
                    i, fmt.plane_fmt[i].bytesperline, fmt.plane_fmt[i].sizeimage,
                    i < maps.size() ? maps[i].size() : size_t(0));
            }
            try {
                auto exported = cap.export_(O_RDONLY | O_CLOEXEC);
                std::fprintf(stderr,
                    "libva-v4l2: TRANSCODE PROBE VDEC EXPBUF success objects=%zu (probe only; fds closed)\n",
                    exported.size());
                for (int fd : exported) close(fd);
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                    "libva-v4l2: TRANSCODE PROBE VDEC EXPBUF unavailable: %s\n", e.what());
            }
            const bool layout_match = surface.conversion == Conversion::None
                && fmt.pixelformat == V4L2_PIX_FMT_NV12M
                && fmt.num_planes == 2 && enc.num_planes == 2
                && fmt.plane_fmt[0].bytesperline == enc.plane_fmt[0].bytesperline
                && fmt.plane_fmt[1].bytesperline == enc.plane_fmt[1].bytesperline
                && fmt.plane_fmt[0].sizeimage >= enc.plane_fmt[0].sizeimage
                && fmt.plane_fmt[1].sizeimage >= enc.plane_fmt[1].sizeimage;
            std::fprintf(stderr,
                "libva-v4l2: TRANSCODE PROBE decoder capture compatible with encoder NV12M=%d "
                "(memory layout only; synchronization not assessed)\n", layout_match);
        } else {
            std::fprintf(stderr,
                "libva-v4l2: TRANSCODE PROBE no decoder capture buffer attached; inspect source mapping path\n");
        }
    }
    surface.encoder_input = true;
    // FFmpeg can create a VA context rounded to a macroblock boundary
    // (1920x1088), while its upload surfaces retain the actual 1920x1080
    // picture.  Do not confuse either with the V4L2 buffer allocation.
    const unsigned coded_width = device.output_format.fmt.pix_mp.width;
    const unsigned coded_height = device.output_format.fmt.pix_mp.height;
    const auto align_up_to = [](unsigned value, unsigned alignment) {
        return (value + alignment - 1) / alignment * alignment;
    };
    const bool valid_surface = surface.format == VA_RT_FORMAT_YUV420
        && surface.width >= visible_width && surface.height >= visible_height;
    const bool first_surface_may_define_crop = !visible_dimensions_locked
        && surface.format == VA_RT_FORMAT_YUV420
        && surface.width > 0 && surface.height > 0
        && surface.width <= static_cast<unsigned>(picture_width)
        && surface.height <= static_cast<unsigned>(picture_height)
        && align_up_to(surface.width, 16) == coded_width
        && align_up_to(surface.height, 32) == coded_height;

    if (!valid_surface && !first_surface_may_define_crop) {
        if (encoder_debug_enabled()) {
            std::fprintf(stderr,
                "libva-v4l2: encoder surface %ux%u is incompatible with visible %ux%u (format=0x%x)\n",
                surface.width, surface.height, visible_width, visible_height, surface.format);
        }
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (!visible_dimensions_locked) {
        if (first_surface_may_define_crop
            && (surface.width < visible_width || surface.height < visible_height)) {
            // Set the visible rectangle before STREAMON.  On this VENC the
            // encoder generates its own SPS; changing just the copy height
            // would still produce a 1088-line H.264 stream.
            v4l2_selection crop = {};
            crop.type = device.output_buf_type;
            crop.target = V4L2_SEL_TGT_CROP;
            crop.r.width = surface.width;
            crop.r.height = surface.height;
            if (ioctl(device.video_fd, VIDIOC_S_SELECTION, &crop) < 0) {
                std::fprintf(stderr,
                    "libva-v4l2: VENC S_SELECTION %ux%u failed: %s\n",
                    surface.width, surface.height, std::strerror(errno));
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            if (crop.r.width != surface.width || crop.r.height != surface.height) {
                std::fprintf(stderr,
                    "libva-v4l2: VENC S_SELECTION returned %ux%u instead of %ux%u\n",
                    crop.r.width, crop.r.height, surface.width, surface.height);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            visible_width = surface.width;
            visible_height = surface.height;
            if (encoder_debug_enabled()) {
                std::fprintf(stderr, "libva-v4l2: VENC visible picture %ux%u, coded buffer %ux%u\n",
                    visible_width, visible_height, coded_width, coded_height);
            }
        }
        visible_dimensions_locked = true;
    }

    if (surface.pending_sync) {
        try {
            waitForConversion(surface);
            surface.pending_sync = false;
        } catch (const std::exception& e) {
            fprintf(stderr, "libva-v4l2: unable to wait for source surface conversion: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (surface.imported_buffer && !surface.converted_buffer) {
        surface.converted_buffer = surface.imported_buffer;
        surface.logical_destination_layout = surface.imported_layout;
    }
    if (!surface.converted_buffer && surface.export_buffer) {
        // DrmBuffer is a write-combined dumb buffer. Re-import its DMA-BUF fd
        // through DmaBuffer so the CPU read below is bracketed by DMA_BUF_SYNC.
        try {
            const int fd = surface.export_buffer->duplicate_fd();
            try {
                surface.converted_buffer = std::make_shared<DmaBuffer>(
                    DmaBuffer::import(fd, surface.export_buffer->size()));
            } catch (...) {
                close(fd);
                throw;
            }
            close(fd);
            surface.logical_destination_layout = surface.export_layout;
        } catch (const std::exception& e) {
            fprintf(stderr, "libva-v4l2: unable to map exported encoder input for CPU read: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    if (!surface.converted_buffer && !surface.destination_buffer && !surface.export_buffer
        && surface.encoder_drm_planes.empty()) {
        try {
            surface.converted_buffer = std::make_shared<DmaBuffer>(
                DmaBuffer::allocate(static_cast<size_t>(surface.width) * surface.height * 3 / 2));
            surface.logical_destination_layout = nv12_layout_with_pitch(surface.width, surface.height);
        } catch (const std::exception& e) {
            fprintf(stderr, "libva-v4l2: unable to allocate NV12 encode surface: %s\n", e.what());
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    if (surface.logical_destination_layout.empty()) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    return VA_STATUS_SUCCESS;
}

void H264EncoderContext::begin_picture()
{
    picture.reset();
    slice_seen = false;
}

VAStatus H264EncoderContext::store_buffer(const Buffer& buffer) const
{
    switch (buffer.type) {
    case VAEncSequenceParameterBufferType:
        if (!buffer.data || buffer.size < sizeof(VAEncSequenceParameterBufferH264)) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        sequence = *reinterpret_cast<const VAEncSequenceParameterBufferH264*>(buffer.data.get());
        controls_dirty = true;
        return VA_STATUS_SUCCESS;

    case VAEncPictureParameterBufferType:
        if (!buffer.data || buffer.size < sizeof(VAEncPictureParameterBufferH264)) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        picture = *reinterpret_cast<const VAEncPictureParameterBufferH264*>(buffer.data.get());
        return VA_STATUS_SUCCESS;

    case VAEncSliceParameterBufferType:
        slice_seen = true;
        return VA_STATUS_SUCCESS;

    // The MT8183 encoder creates SPS/PPS itself.  Packed headers and optional
    // tuning payloads are accepted but not interpreted in this first version.
    case VAEncMiscParameterBufferType: {
        const size_t header_size = offsetof(VAEncMiscParameterBuffer, data);
        if (!buffer.data || buffer.size < header_size) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        const auto* misc = reinterpret_cast<const VAEncMiscParameterBuffer*>(buffer.data.get());
        if (misc->type == VAEncMiscParameterTypeRateControl) {
            if (buffer.size < header_size + sizeof(VAEncMiscParameterRateControl)) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const auto* rate_control = reinterpret_cast<const VAEncMiscParameterRateControl*>(misc->data);
            if (encoder_debug_enabled()) {
                fprintf(stderr, "libva-v4l2: H264 VA RC misc bits_per_second=%u\n",
                    rate_control->bits_per_second);
            }
            if (rate_control->bits_per_second) {
                target_bitrate = rate_control->bits_per_second;
            }
            if (rate_control->max_qp > 0 && rate_control->max_qp <= 51) {
                maximum_qp = rate_control->max_qp;
            }
            controls_dirty = true;
        }
        if (misc->type == VAEncMiscParameterTypeFrameRate) {
            if (buffer.size < header_size + sizeof(VAEncMiscParameterFrameRate)) {
                return VA_STATUS_ERROR_INVALID_BUFFER;
            }
            const auto* frame_rate = reinterpret_cast<const VAEncMiscParameterFrameRate*>(misc->data);
            const unsigned numerator = frame_rate->framerate & 0xffffu;
            const unsigned denominator = (frame_rate->framerate >> 16) ? (frame_rate->framerate >> 16) : 1u;
            if (numerator == 0) {
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            if (fps_numerator != numerator || fps_denominator != denominator) {
                fps_numerator = numerator;
                fps_denominator = denominator;
                framerate_dirty = true;
            }
        }
        return VA_STATUS_SUCCESS;
    }

    case VAEncPackedHeaderParameterBufferType:
    case VAEncPackedHeaderDataBufferType:
    case VAEncQPBufferType:
        return VA_STATUS_SUCCESS;

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
}

void H264EncoderContext::queue_capture(unsigned index)
{
    auto& mapped = capture_buffers.at(index);
    if (mapped.queued) {
        return;
    }
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes = {};
    v4l2_buffer buffer = {};
    buffer.type = device.capture_buf_type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.m.planes = planes.data();
    buffer.length = mapped.planes.size();
    for (unsigned i = 0; i < mapped.planes.size(); i++) {
        planes[i].length = mapped.planes[i].size();
        planes[i].bytesused = 0;
    }
    ioctl_or_throw(device.video_fd, VIDIOC_QBUF, &buffer, "queueing H.264 capture buffer");
    mapped.queued = true;
}

void H264EncoderContext::queue_input(unsigned index, Surface& surface)
{
    auto& mapped = input_buffers.at(index);
    auto source_maps = surface.picture_mapping();
    const auto& layout = surface.logical_destination_layout;
    if (mapped.planes.size() != 2 || layout.size() < 2) {
        throw std::runtime_error("the encoder expects a two-plane NV12 surface");
    }

    const auto& output_format = device.output_format.fmt.pix_mp;
    if (encoder_debug_enabled() && frame_number == 0) {
        std::fprintf(stderr,
            "libva-v4l2: VENC input selection: dmabuf=%d direct_planes=%zu converted=%d imported=%d layout_planes=%zu\n",
            use_dmabuf_input, surface.encoder_drm_planes.size(), static_cast<bool>(surface.converted_buffer),
            static_cast<bool>(surface.imported_buffer), layout.size());
    }
    if (use_dmabuf_input && surface.encoder_drm_planes.size() == 2) {
        const auto& direct = surface.encoder_drm_planes;
        const bool layout_compatible = layout[0].physical_plane_index == 0
            && layout[1].physical_plane_index == 1
            && layout[0].offset == 0 && layout[1].offset == 0
            && layout[0].pitch == output_format.plane_fmt[0].bytesperline
            && layout[1].pitch == output_format.plane_fmt[1].bytesperline
            && direct[0]->size() >= output_format.plane_fmt[0].sizeimage
            && direct[1]->size() >= output_format.plane_fmt[1].sizeimage;
        if (layout_compatible) {
            std::array<v4l2_plane, VIDEO_MAX_PLANES> planes = {};
            v4l2_buffer buffer = {};
            buffer.type = device.output_buf_type;
            buffer.memory = V4L2_MEMORY_DMABUF;
            buffer.index = index;
            buffer.m.planes = planes.data();
            buffer.length = 2;
            unsigned opened = 0;
            try {
                for (unsigned i = 0; i < 2; ++i) {
                    planes[i].length = direct[i]->size();
                    planes[i].bytesused = output_format.plane_fmt[i].sizeimage;
                    planes[i].m.fd = direct[i]->duplicate_fd();
                    ++opened;
                }
            } catch (...) {
                for (unsigned i = 0; i < opened; ++i) close(planes[i].m.fd);
                throw;
            }
            const int result = ioctl(device.video_fd, VIDIOC_QBUF, &buffer);
            const int error = errno;
            for (unsigned i = 0; i < opened; ++i) close(planes[i].m.fd);
            if (result == 0) {
                mapped.queued = true;
                ++direct_prime_qbuf;
                if (encoder_debug_enabled() && direct_prime_qbuf == 1) {
                    std::fprintf(stderr,
                        "libva-v4l2: VENC first successful direct PRIME QBUF at encoded frame %llu (no encoder staging copy)\n",
                        static_cast<unsigned long long>(frame_number));
                }
                return;
            }
            ++direct_import_rejections;
            if (!streaming) {
                if (encoder_debug_enabled()) {
                    std::fprintf(stderr, "libva-v4l2: direct PRIME QBUF rejected errno=%d (%s), MMAP fallback\n",
                        error, std::strerror(error));
                }
                switch_input_to_mmap();
                queue_input(index, surface);
                return;
            }
            throw std::system_error(error, std::generic_category(), "queueing direct PRIME NV12M");
        }
        ++direct_layout_fallbacks;
        if (encoder_debug_enabled() && direct_layout_fallbacks == 1) {
            std::fprintf(stderr, "libva-v4l2: direct PRIME layout not V4L2-compatible; staging copy\n");
        }
    }

    // Convert MM21 directly into the two VENC staging DMA-BUF planes, avoiding
    // the intermediate linear NV12 and the later full-frame copy. This is an
    // opt-in transcode-only path; ordinary decode and hwupload are unaffected.
    bool direct_converted = false;
    if (surface.deferred_encoder_conversion) {
        try {
            if (mapped.planes.size() != 2 || !use_dmabuf_input
                || surface.conversion != Conversion::MM21ToNV12
                || visible_width == 0 || visible_height == 0)
                throw std::runtime_error("direct conversion not applicable to this encoder layout");
            const bool padding_only = [] {
                const char* env = std::getenv("LIBVA_V4L2_TRANSCODE_PADDING_ONLY");
                return env && std::strcmp(env, "1") == 0;
            }();
            // Select one path per invocation; never perform a shadow conversion
            // in the same frame because that would perturb caches and bandwidth.
            const char* mode_env = std::getenv("LIBVA_V4L2_TRANSCODE_MEMORY_MODE");
            const bool ramcopy = mode_env && std::strcmp(mode_env, "ramcopy") == 0;
            const auto& y_format = output_format.plane_fmt[0];
            const auto& uv_format = output_format.plane_fmt[1];
            if (y_format.sizeimage > mapped.planes[0].size()
                || uv_format.sizeimage > mapped.planes[1].size())
                throw std::runtime_error("NV12M plane is smaller than negotiated sizeimage");
            // Start the end-to-end timing before either branch allocates memory,
            // applies padding or synchronizes DMA-BUFs.
            const uint64_t total_start_wall = wall_ns();
            const uint64_t total_start_cpu = thread_cpu_ns();
            const auto start = std::chrono::steady_clock::now();
            const unsigned group_tiles = [] {
                const char* env = std::getenv("LIBVA_V4L2_TRANSCODE_TILE_GROUP");
                if (env && std::strcmp(env, "4") == 0) return 4u;
                if (env && std::strcmp(env, "8") == 0) return 8u;
                return 1u;
            }();
            // 'tile' is the original implementation; 'row' writes each
            // destination band in ascending address order. Opt-in only.
            const char* order_env = std::getenv("LIBVA_V4L2_TRANSCODE_DETILE_ORDER");
            const bool row_major = order_env && std::strcmp(order_env, "row") == 0;
            // Optional source-only hardware prefetch experiment (0=off, 1/2=tile lookahead).
            const unsigned prefetch_tiles = [] {
                const char* env = std::getenv("LIBVA_V4L2_TRANSCODE_PREFETCH");
                if (env && std::strcmp(env, "1") == 0) return 1u;
                if (env && std::strcmp(env, "2") == 0) return 2u;
                return 0u;
            }();
            static bool prefetch_reported = false;
            if (!prefetch_reported && encoder_debug_enabled()) {
                std::fprintf(stderr, "libva-v4l2: TRANSCODE PREFETCH source_tiles=%u (0=off, 1/2=lookahead)\n",
                    prefetch_tiles);
                prefetch_reported = true;
            }
            // Use the same padding policy and detiling function for both paths.
            auto prepare_padding = [&](std::span<uint8_t> yp, std::span<uint8_t> uvp) {
                if (padding_only) {
                    fill_nv12m_padding(yp, y_format.bytesperline,
                        visible_width, visible_height, 0x10);
                    fill_nv12m_padding(uvp, uv_format.bytesperline,
                        (visible_width + 1u) & ~1u, (visible_height + 1u) / 2, 0x80);
                } else {
                    std::fill(yp.begin(), yp.end(), 0x10);
                    std::fill(uvp.begin(), uvp.end(), 0x80);
                }
            };
            auto detile = [&](std::span<uint8_t> yp, std::span<uint8_t> uvp) {
                detileDecoderIntoNV12M(surface, yp, y_format.bytesperline,
                    uvp, uv_format.bytesperline, visible_width, visible_height, group_tiles, row_major, prefetch_tiles);
            };
            if (ramcopy) {
                // Allocate ordinary reusable RAM at precisely the same plane sizes.
                // The entire RAM planes are copied into the DMA-BUF, including
                // padding initialized by prepare_padding().
                benchmark_ram_y.resize(mapped.planes[0].size());
                benchmark_ram_uv.resize(mapped.planes[1].size());
                const std::span<uint8_t> ram_y(benchmark_ram_y);
                const std::span<uint8_t> ram_uv(benchmark_ram_uv);
                prepare_padding(ram_y, ram_uv);
                const auto after_padding = std::chrono::steady_clock::now();
                const uint64_t detile_start_wall = wall_ns();
                const uint64_t detile_start_cpu = thread_cpu_ns();
                detile(ram_y, ram_uv);
                benchmark_ram_detile_wall_ns += wall_ns() - detile_start_wall;
                benchmark_ram_detile_cpu_ns += thread_cpu_ns() - detile_start_cpu;
                const auto after_detile = std::chrono::steady_clock::now();
                // Include DMA-BUF START/END sync in the copy timer.
                const uint64_t copy_start_wall = wall_ns();
                const uint64_t copy_start_cpu = thread_cpu_ns();
                {
                    DmaBufCpuWriteGuard write_guard(mapped.dma_planes);
                    std::memcpy(mapped.planes[0].data(), ram_y.data(), ram_y.size());
                    std::memcpy(mapped.planes[1].data(), ram_uv.data(), ram_uv.size());
                    write_guard.end();
                }
                benchmark_dma_copy_wall_ns += wall_ns() - copy_start_wall;
                benchmark_dma_copy_cpu_ns += thread_cpu_ns() - copy_start_cpu;
                benchmark_ramcopy_wall_ns += wall_ns() - total_start_wall;
                benchmark_ramcopy_cpu_ns += thread_cpu_ns() - total_start_cpu;
                ++benchmark_ramcopy_frames;
                direct_convert_padding_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(after_padding - start).count();
                direct_convert_detile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(after_detile - after_padding).count();
            } else {
                uint64_t detile_start_cpu = 0;
                {
                    DmaBufCpuWriteGuard write_guard(mapped.dma_planes);
                    prepare_padding(mapped.planes[0], mapped.planes[1]);
                    const auto after_padding = std::chrono::steady_clock::now();
                    detile_start_cpu = thread_cpu_ns();
                    detile(mapped.planes[0], mapped.planes[1]);
                    benchmark_dma_detile_cpu_ns += thread_cpu_ns() - detile_start_cpu;
                    const auto after_detile = std::chrono::steady_clock::now();
                    direct_convert_padding_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(after_padding - start).count();
                    direct_convert_detile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(after_detile - after_padding).count();
                    write_guard.end();
                }
                benchmark_direct_wall_ns += wall_ns() - total_start_wall;
                benchmark_direct_cpu_ns += thread_cpu_ns() - total_start_cpu;
                ++benchmark_direct_frames;
            }
            direct_convert_total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (padding_only) ++direct_convert_padding_only_frames;
            direct_converted = true;
            ++direct_convert_frames;
            surface.deferred_encoder_conversion = false;
            if (direct_convert_frames == 1) {
                std::fprintf(stderr, "libva-v4l2: TRANSCODE DIRECT CONVERT MM21 -> NV12M enabled (one CPU conversion, no NV12 staging copy)\n");
                const char* group = std::getenv("LIBVA_V4L2_TRANSCODE_TILE_GROUP");
                std::fprintf(stderr, "libva-v4l2: TRANSCODE TILE GROUP requested=%s (1=original; 4/8=grouped)\n", group ? group : "1");
                std::fprintf(stderr, "libva-v4l2: TRANSCODE MEMORY MODE=%s (direct or ramcopy)\n", ramcopy ? "ramcopy" : "direct");
                std::fprintf(stderr, "libva-v4l2: TRANSCODE DETILE ORDER=%s (tile or row)\n", row_major ? "row" : "tile");
            }
        } catch (const std::exception& e) {
            ++direct_convert_fallbacks;
            std::fprintf(stderr, "libva-v4l2: TRANSCODE DIRECT CONVERT fallback: %s\n", e.what());
            materializeDeferredConversion(surface);
        }
    }

    const unsigned coded_height = device.output_format.fmt.pix_mp.height;
    // Copy only the VA context's visible rectangle.  The source may have
    // extra aligned rows/columns; V4L2 padding is filled independently.
    const unsigned source_width = visible_width;
    const unsigned source_height = visible_height;
    const std::array<unsigned, 2> row_counts = { coded_height, coded_height / 2 };
    const std::array<uint8_t, 2> padding = { 0x10, 0x80 };
    if (!direct_converted) {
    std::array<DmaBuffer*, 2> sync_buffers = {};
    if (surface.imported_buffer) {
        sync_buffers[0] = surface.imported_buffer.get();
        sync_buffers[0]->begin_cpu_access(false);
    } else if (surface.converted_buffer) {
        sync_buffers[0] = surface.converted_buffer.get();
        sync_buffers[0]->begin_cpu_access(false);
    }

    try {
        // If a direct surface needs a staging fallback, synchronize its two
        // source planes for CPU reading as well.
        std::vector<int> source_prime_fds;
        source_prime_fds.reserve(surface.encoder_drm_planes.size());
        struct SourcePrimeGuard {
            std::vector<int>& fds;
            int end_impl() noexcept {
                int first_error = 0;
                for (int fd : fds) {
                    dma_buf_sync sync = {};
                    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && !first_error)
                        first_error = errno;
                    close(fd);
                }
                fds.clear();
                return first_error;
            }
            void finish() {
                const int error = end_impl();
                if (error)
                    throw std::system_error(error, std::generic_category(), "ending PRIME DMA-BUF CPU read");
            }
            ~SourcePrimeGuard() noexcept {
                const int error = end_impl();
                if (error)
                    std::fprintf(stderr, "libva-v4l2: PRIME DMA-BUF CPU read END during cleanup failed: %s\n", std::strerror(error));
            }
        } source_prime_guard { source_prime_fds };
        for (const auto& plane : surface.encoder_drm_planes) {
            int fd = plane->duplicate_fd();
            dma_buf_sync sync = {};
            sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
            if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
                int error = errno;
                close(fd);
                throw std::system_error(error, std::generic_category(), "starting PRIME CPU read");
            }
            source_prime_fds.push_back(fd);
        }
        // Scope the writer synchronization to the CPU staging copy, before
        // the V4L2 device is allowed to read the imported buffers.
        DmaBufCpuWriteGuard dma_write_guard(mapped.dma_planes);
        for (unsigned plane = 0; plane < 2; plane++) {
            auto destination = mapped.planes[plane];
            std::fill(destination.begin(), destination.end(), padding[plane]);

            const auto& logical = layout[plane];
            const unsigned source_rows = plane == 0 ? source_height : source_height / 2;
            const unsigned dest_pitch = output_format.plane_fmt[plane].bytesperline;
            const unsigned source_pitch = logical.pitch;
            const unsigned row_bytes = source_width;
            if (source_pitch < row_bytes || dest_pitch < row_bytes || logical.physical_plane_index >= source_maps.size()
                || logical.size < row_bytes
                || static_cast<size_t>(source_rows - 1) > (logical.size - row_bytes) / source_pitch) {
                throw std::runtime_error("NV12 plane pitch or size does not match the encoder format");
            }
            if (output_format.plane_fmt[plane].sizeimage > destination.size()) {
                throw std::runtime_error("V4L2 input plane is smaller than its negotiated sizeimage");
            }
            if (source_rows > row_counts[plane]) {
                throw std::runtime_error("source surface is taller than the configured encoder frame");
            }
            const auto source = source_maps[logical.physical_plane_index];
            for (unsigned row = 0; row < source_rows; row++) {
                const size_t source_offset = static_cast<size_t>(logical.offset) + static_cast<size_t>(row) * source_pitch;
                const size_t destination_offset = static_cast<size_t>(row) * dest_pitch;
                if (source_offset + row_bytes > source.size() || destination_offset + row_bytes > destination.size()) {
                    throw std::runtime_error("NV12 source or destination plane is too small");
                }
                memcpy(destination.data() + destination_offset, source.data() + source_offset, row_bytes);
            }
        }
        dma_write_guard.end();
        source_prime_guard.finish();
    } catch (...) {
        if (sync_buffers[0]) {
            sync_buffers[0]->end_cpu_access(false);
        }
        throw;
    }
    if (sync_buffers[0]) {
        sync_buffers[0]->end_cpu_access(false);
    }

    } // ordinary NV12 staging-copy path

    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes = {};
    v4l2_buffer buffer = {};
    buffer.type = device.output_buf_type;
    buffer.memory = use_dmabuf_input ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.m.planes = planes.data();
    buffer.length = mapped.planes.size();
    unsigned duplicated_fds = 0;
    try {
        for (unsigned i = 0; i < mapped.planes.size(); i++) {
            planes[i].length = mapped.planes[i].size();
            planes[i].bytesused = output_format.plane_fmt[i].sizeimage;
            planes[i].data_offset = 0;
            if (use_dmabuf_input) {
                planes[i].m.fd = mapped.dma_planes.at(i)->duplicate_fd();
                ++duplicated_fds;
            }
        }
    } catch (...) {
        for (unsigned i = 0; i < duplicated_fds; ++i) {
            close(planes[i].m.fd);
        }
        throw;
    }
    // vb2 imports the DMA-BUF references during QBUF. Keep their owning
    // DrmBuffer objects alive until DQBUF, close only temporary dup() fds.
    const int result = ioctl(device.video_fd, VIDIOC_QBUF, &buffer);
    const int saved_errno = errno;
    for (unsigned i = 0; i < duplicated_fds; ++i) {
        close(planes[i].m.fd);
    }
    if (result < 0) {
        if (use_dmabuf_input && !streaming) {
            // The direct-converted DMA planes are about to be discarded.
            // Recreate the ordinary linear NV12 image before MMAP retry.
            if (direct_converted) {
                surface.deferred_encoder_conversion = true;
                materializeDeferredConversion(surface);
                ++direct_convert_fallbacks;
            }
            switch_input_to_mmap();
            queue_input(index, surface);
            return;
        }
        throw std::system_error(saved_errno, std::generic_category(), "queueing raw NV12 input buffer");
    }
    mapped.queued = true;
    if (use_dmabuf_input) {
        ++dmabuf_staging_qbuf;
    } else {
        ++mmap_qbuf;
    }
}

void H264EncoderContext::start_streaming()
{
    if (streaming) {
        return;
    }
    auto output_type = device.output_buf_type;
    auto capture_type = device.capture_buf_type;
    ioctl_or_throw(device.video_fd, VIDIOC_STREAMON, &capture_type, "starting encoder CAPTURE queue");
    try {
        ioctl_or_throw(device.video_fd, VIDIOC_STREAMON, &output_type, "starting encoder OUTPUT queue");
        streaming = true;
    } catch (...) {
        ioctl(device.video_fd, VIDIOC_STREAMOFF, &capture_type);
        throw;
    }
}

void H264EncoderContext::dequeue_buffer(v4l2_buf_type type, std::vector<MappedBuffer>& buffers, unsigned& index,
    std::vector<uint8_t>* payload)
{
    const short events = V4L2_TYPE_IS_CAPTURE(type) ? (POLLIN | POLLRDNORM) : (POLLOUT | POLLWRNORM);
    while (true) {
        std::array<v4l2_plane, VIDEO_MAX_PLANES> planes = {};
        v4l2_buffer buffer = {};
        buffer.type = type;
        buffer.memory = type == device.output_buf_type && use_dmabuf_input
            ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        buffer.m.planes = planes.data();
        buffer.length = VIDEO_MAX_PLANES;
        if (ioctl(device.video_fd, VIDIOC_DQBUF, &buffer) == 0) {
            index = buffer.index;
            if (index >= buffers.size()) {
                throw std::runtime_error("V4L2 encoder dequeued an invalid buffer index");
            }
            buffers[index].queued = false;
            if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
                throw std::runtime_error("V4L2 encoder returned a buffer marked with ERROR");
            }
            if (payload) {
                if (buffer.length < 1 || planes[0].bytesused < planes[0].data_offset) {
                    throw std::runtime_error("V4L2 encoder returned an invalid CAPTURE payload");
                }
                const size_t size = planes[0].bytesused - planes[0].data_offset;
                const auto mapped = buffers[index].planes[0];
                if (planes[0].data_offset + size > mapped.size()) {
                    throw std::runtime_error("V4L2 encoder CAPTURE payload exceeds its mapped buffer");
                }
                payload->assign(mapped.begin() + planes[0].data_offset,
                    mapped.begin() + planes[0].data_offset + size);
            }
            return;
        }
        if (errno != EAGAIN && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "dequeueing V4L2 encoder buffer");
        }

        pollfd descriptor = { .fd = device.video_fd, .events = events, .revents = 0 };
        const int ready = poll(&descriptor, 1, kPollTimeoutMs);
        if (ready == 0) {
            throw std::runtime_error("timed out waiting for the MT8183 encoder");
        }
        if (ready < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "polling the MT8183 encoder");
        }
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw std::runtime_error("the MT8183 encoder reported a poll error");
        }
    }
}

VAStatus H264EncoderContext::encode_picture(VADriverContextP va_context)
{
    if (!sequence || !picture) {
        error_log(va_context, "H.264 encode picture is missing sequence or picture parameters\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!driver_data->surfaces.contains(render_surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    auto coded = driver_data->buffers.find(picture->coded_buf);
    if (coded == driver_data->buffers.end() || coded->second.type != VAEncCodedBufferType) {
        error_log(va_context, "H.264 picture references an invalid coded buffer\n");
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    try {
        const auto setup_result = prepare_surface(render_surface_id);
        if (setup_result != VA_STATUS_SUCCESS) {
            return setup_result;
        }
        if (set_controls() < 0) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (picture->pic_fields.bits.idr_pic_flag && set_control(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0) < 0) {
            fprintf(stderr, "libva-v4l2: unable to request IDR frame: %s\n", strerror(errno));
        }

        auto& surface = driver_data->surfaces.at(render_surface_id);

        const unsigned input_index = next_input++ % input_buffers.size();
        for (unsigned i = 0; i < capture_buffers.size(); i++) {
            queue_capture(i);
        }
        queue_input(input_index, surface);
        start_streaming();

        std::vector<uint8_t> encoded;
        unsigned capture_index = 0;
        unsigned input_done_index = input_index;
        bool capture_done = false;
        bool input_done = false;
        while (!capture_done || !input_done) {
            if (!capture_done) {
                try {
                    dequeue_buffer(device.capture_buf_type, capture_buffers, capture_index, &encoded);
                    capture_done = true;
                    queue_capture(capture_index);
                } catch (const std::system_error& e) {
                    if (e.code().value() != EAGAIN) {
                        throw;
                    }
                }
            }
            if (!input_done) {
                try {
                    dequeue_buffer(device.output_buf_type, input_buffers, input_done_index);
                    input_done = true;
                } catch (const std::system_error& e) {
                    if (e.code().value() != EAGAIN) {
                        throw;
                    }
                }
            }
            if (!capture_done || !input_done) {
                // dequeue_buffer performs its own poll. If the other queue is
                // not ready yet, the next iteration waits on that queue.
                continue;
            }
        }

        if (encoded.empty()) {
            throw std::runtime_error("the MT8183 encoder returned an empty H.264 frame");
        }
        auto& coded_buffer = coded->second;
        if (encoded.size() > coded_buffer.coded_data.size()) {
            error_log(va_context, "H.264 output (%zu bytes) exceeds coded buffer capacity (%zu bytes)\n",
                encoded.size(), coded_buffer.coded_data.size());
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        }
        std::copy(encoded.begin(), encoded.end(), coded_buffer.coded_data.begin());
        coded_buffer.coded_segment = {};
        coded_buffer.coded_segment.size = encoded.size();
        coded_buffer.coded_segment.bit_offset = 0;
        coded_buffer.coded_segment.buf = coded_buffer.coded_data.data();
        coded_buffer.coded_segment.next = nullptr;
        frame_number++;
        picture.reset();
        slice_seen = false;
        return VA_STATUS_SUCCESS;
    } catch (const std::exception& e) {
        stop_streaming();
        error_log(va_context, "MT8183 H.264 encode failed: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
}
