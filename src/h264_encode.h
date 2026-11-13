/*
 * H.264 stateful V4L2 M2M encoder support for MediaTek VENC.
 *
 * The encoder uses the regular stateful M2M queue layout: raw NV12M on
 * OUTPUT_MPLANE and H.264 on CAPTURE_MPLANE.  This is deliberately kept
 * separate from H264Context, which implements stateless H.264 decoding.
 */
#pragma once

#include <optional>
#include <set>
#include <span>
#include <vector>

extern "C" {
#include <linux/videodev2.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
}

#include "context.h"

struct Surface;
class DrmBuffer;

class H264EncoderContext final : public Context {
public:
    static std::set<VAProfile> supported_profiles(const V4L2M2MDevice& device);
    static bool supports_nv12_surfaces(const V4L2M2MDevice& device);

    H264EncoderContext(DriverData* driver_data, V4L2M2MDevice& device, VAProfile profile,
        int picture_width, int picture_height);
    ~H264EncoderContext() override;

    VAStatus store_buffer(const Buffer& buffer) const override;
    int set_controls() override;
    VAStatus prepare_surface(VASurfaceID surface_id);
    void begin_picture();
    VAStatus encode_picture(VADriverContextP va_context);

private:
    struct MappedBuffer {
        std::vector<std::span<uint8_t>> planes;
        // Owns the two separately allocated DRM PRIME planes in DMABUF mode.
        std::vector<std::unique_ptr<DrmBuffer>> dma_planes;
        bool queued = false;

        MappedBuffer() = default;
        MappedBuffer(const MappedBuffer&) = delete;
        MappedBuffer& operator=(const MappedBuffer&) = delete;
        MappedBuffer(MappedBuffer&& other) noexcept;
        MappedBuffer& operator=(MappedBuffer&& other) noexcept;
        ~MappedBuffer();
    };

    void set_capture_format();
    void allocate_queue(v4l2_buf_type type, std::vector<MappedBuffer>& buffers, unsigned count);
    void release_queues() noexcept;
    void switch_input_to_mmap();
    void allocate_dmabuf_input();
    void queue_capture(unsigned index);
    void queue_input(unsigned index, Surface& surface);
    void dequeue_buffer(v4l2_buf_type type, std::vector<MappedBuffer>& buffers, unsigned& index,
        std::vector<uint8_t>* payload = nullptr);
    void start_streaming();
    void stop_streaming() noexcept;
    int set_control(uint32_t id, int32_t value) const;
    unsigned h264_level_control() const;
    unsigned h264_profile_control() const;

    VAProfile va_profile;
    unsigned visible_width;
    unsigned visible_height;
    bool visible_dimensions_locked = false;
    mutable std::optional<VAEncSequenceParameterBufferH264> sequence;
    mutable std::optional<VAEncPictureParameterBufferH264> picture;
    mutable bool slice_seen = false;
    mutable bool controls_dirty = true;
    mutable std::optional<unsigned> target_bitrate;
    mutable std::optional<unsigned> maximum_qp;
    mutable unsigned fps_numerator = 30;
    mutable unsigned fps_denominator = 1;
    bool framerate_applied = false;
    mutable bool framerate_dirty = false;
    bool controls_applied = false;
    bool streaming = false;
    bool use_dmabuf_input = false;
    bool transcode_probe_logged = false;
    unsigned next_input = 0;
    uint64_t frame_number = 0;
    // Count only successful V4L2 OUTPUT QBUF calls; these counters do not
    // include failed attempts or capture queue operations.
    uint64_t direct_prime_qbuf = 0;
    uint64_t dmabuf_staging_qbuf = 0;
    uint64_t mmap_qbuf = 0;
    uint64_t direct_layout_fallbacks = 0;
    uint64_t direct_import_rejections = 0;
    std::vector<MappedBuffer> input_buffers;
    unsigned long long direct_convert_frames = 0;
    unsigned long long direct_convert_fallbacks = 0;
    unsigned long long direct_convert_padding_only_frames = 0;
    uint64_t direct_convert_padding_ns = 0;
    uint64_t direct_convert_detile_ns = 0;
    uint64_t direct_convert_total_ns = 0;
    // Memory-path benchmark: direct DMA-BUF vs RAM detile + linear DMA-BUF copy.
    std::vector<uint8_t> benchmark_ram_y;
    std::vector<uint8_t> benchmark_ram_uv;
    uint64_t benchmark_direct_frames = 0;
    uint64_t benchmark_ramcopy_frames = 0;
    uint64_t benchmark_ram_detile_wall_ns = 0;
    uint64_t benchmark_ram_detile_cpu_ns = 0;
    uint64_t benchmark_dma_detile_cpu_ns = 0;
    uint64_t benchmark_dma_copy_wall_ns = 0;
    uint64_t benchmark_dma_copy_cpu_ns = 0;
    uint64_t benchmark_direct_wall_ns = 0;
    uint64_t benchmark_ramcopy_wall_ns = 0;
    uint64_t benchmark_direct_cpu_ns = 0;
    uint64_t benchmark_ramcopy_cpu_ns = 0;
    std::vector<MappedBuffer> capture_buffers;
};
