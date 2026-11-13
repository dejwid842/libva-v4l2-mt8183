/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
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

#include "surface.h"
#include "encoder_debug_flag.h"

#include <algorithm>
#include <chrono>
#include <cassert>
#include <future>
#include <mutex>
#include <optional>
#include <system_error>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libdrm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include <cstdio>
#include <string>
#include "convert.h"
#include "debug.h"
#include "drm.h"
#include "mdp3.h"
#include "driver.h"
#include "format.h"
#include "h264_encode.h"
#include "media.h"
#include "utils.h"
#include "v4l2.h"

namespace {

decltype(formats)::const_iterator matching_format(const V4L2M2MDevice& device, uint32_t format)
{
    return std::ranges::find_if(formats, [&](auto&& f) {
        return f.va.rt_format == format && device.format_supported(device.capture_buf_type, f.v4l2.format);
    });
}

unsigned align_up(unsigned value, unsigned alignment) { return (value + alignment - 1) / alignment * alignment; }

// Check that a logical plane contains every byte of the requested rows, including its offset in the backing buffer.
// This deliberately rejects truncated planes instead of silently shortening a picture.
bool logical_plane_fits(const LogicalPlane& plane, size_t buffer_size, unsigned rows, unsigned row_bytes)
{
    if (rows == 0 || row_bytes == 0 || plane.pitch < row_bytes || plane.offset > buffer_size
        || plane.size > buffer_size - plane.offset || plane.size < row_bytes) {
        return false;
    }
    return static_cast<size_t>(rows - 1) <= (plane.size - row_bytes) / plane.pitch;
}

// Row length in bytes of a plane of a tiled capture buffer.
unsigned tiled_stride(const v4l2_pix_format_mplane& format, unsigned plane)
{
    const auto stride = format.plane_fmt[plane].bytesperline;
    return stride ? stride : align_up(format.width, mm21::tile_width);
}

// Ensure that the converter never reads outside of the mapped capture buffer planes.
void check_mm21_mapping(const v4l2_pix_format_mplane& format, const std::vector<std::span<uint8_t>>& mapping)
{
    if (format.num_planes != 2 || mapping.size() != 2) {
        throw std::runtime_error("MM21 capture buffers are expected to consist of two planes");
    }
    const unsigned heights[2] = { format.height, (format.height + 1) / 2 };
    const unsigned tile_heights[2] = { mm21::luma_tile_height, mm21::chroma_tile_height };
    for (unsigned plane = 0; plane < 2; plane++) {
        const auto stride = tiled_stride(format, plane);
        if (stride % mm21::tile_width != 0 || stride < align_up(format.width, mm21::tile_width)) {
            throw std::runtime_error("Unsupported row length of MM21 capture buffer");
        }
        if (mapping[plane].size() < mm21::plane_min_size(stride, heights[plane], tile_heights[plane])) {
            throw std::runtime_error("MM21 capture buffer plane is too small for its tiled layout");
        }
    }
}

// Layout of the linear NV12 buffer receiving a converted picture.
BufferLayout converted_layout(const v4l2_pix_format_mplane& format)
{
    // Most GPUs and display engines require (at least) 64 byte aligned pitches for imported buffers.
    return nv12_layout_with_pitch(align_up(format.width, 64), format.height);
}

// Everything the conversion of one picture needs, collected beforehand, so that it can run in another thread without
// touching state which the rest of the driver changes in the meantime.
struct ConversionJob {
    Conversion conversion = Conversion::None;
    std::shared_ptr<DmaBuffer> target;
    std::vector<std::span<uint8_t>> source; // planes of the V4L2 capture buffer
    unsigned source_stride[2] = {};
    unsigned width = 0, height = 0;
    LogicalPlane luma = {}, chroma = {};
    std::string description; // of the driver's format, for diagnostics
    // The buffer exported for the surface, if there is one: it gets a copy of the picture too.
    std::shared_ptr<DrmBuffer> export_target;
    BufferLayout export_layout;
};

// With LIBVA_V4L2_DUMP=<directory> the raw capture planes (and the converted result) of the first pictures are
// written to files, to help with analysing layout problems of new hardware.
void dump_picture(const ConversionJob& job)
{
    static std::mutex mutex;
    static unsigned dumped = 0;
    const char* directory = getenv("LIBVA_V4L2_DUMP");

    std::lock_guard<std::mutex> guard(mutex);
    if (!directory || !*directory || dumped >= 3) {
        return;
    }

    const auto write = [&](const char* suffix, const uint8_t* data, size_t size) {
        const std::string path = std::string(directory) + "/frame" + std::to_string(dumped) + suffix;
        if (FILE* file = fopen(path.c_str(), "wb")) {
            fwrite(data, 1, size, file);
            fclose(file);
        }
    };

    write(".info.txt", reinterpret_cast<const uint8_t*>(job.description.data()), job.description.size());
    for (unsigned plane = 0; plane < job.source.size(); plane++) {
        write((".plane" + std::to_string(plane) + ".raw").c_str(), job.source[plane].data(), job.source[plane].size());
    }
    write(".nv12", job.target->data(), job.target->size());
    dumped++;
}

// Collects what is needed to convert the decoded picture of the (dequeued) capture buffer.
ConversionJob prepare_conversion(const Surface& surface)
{
    const auto& capture = surface.destination_buffer->get();

    ConversionJob job;
    job.conversion = surface.conversion;
    job.target = surface.converted_buffer;
    job.luma = surface.logical_destination_layout.at(0);
    job.chroma = surface.logical_destination_layout.at(1);

    // The driver may have adjusted the format since it was set, so don't rely on what was cached back then.
    job.description = capture.owner().refresh_capture_format();
    const auto& format = capture.owner().capture_format.fmt.pix_mp;

    switch (job.conversion) {
    case Conversion::MM21ToNV12: {
        job.source = capture.mapping();
        check_mm21_mapping(format, job.source);

        job.source_stride[0] = tiled_stride(format, 0);
        job.source_stride[1] = tiled_stride(format, 1);

        if (!job.target) {
            throw std::runtime_error("the converted picture has no destination buffer");
        }
        // V4L2 may adjust S_FMT. Never treat a smaller returned CAPTURE format as a shorter but valid frame.
        if (format.width < surface.width || format.height < surface.height) {
            throw ResolutionNotSupported("V4L2 CAPTURE format " + std::to_string(format.width) + "x"
                + std::to_string(format.height) + " is smaller than the VA surface "
                + std::to_string(surface.width) + "x" + std::to_string(surface.height));
        }

        // Convert the complete VA surface. The coded V4L2 dimensions may include extra alignment padding, but the
        // destination layout must still hold every luma and chroma row visible through VA-API.
        job.width = surface.width;
        job.height = surface.height;
        const unsigned chroma_width = (job.width + 1) & ~1u;
        const unsigned chroma_rows = (job.height + 1) / 2;
        if (!logical_plane_fits(job.luma, job.target->size(), job.height, job.width)
            || !logical_plane_fits(job.chroma, job.target->size(), chroma_rows, chroma_width)) {
            throw std::runtime_error("the complete NV12 picture does not fit into its destination buffer");
        }
        job.export_target = surface.export_buffer;
        job.export_layout = surface.export_layout;
        break;
    }
    case Conversion::None:
        break;
    }

    return job;
}

// The buffer which was exported for the surface has to show what was decoded into it.
void copy_to_export(const ConversionJob& job)
{
    if (!job.export_target) {
        return;
    }
    if (job.export_layout.size() != 2) {
        throw std::runtime_error("the exported NV12 buffer does not describe two planes");
    }

    const auto started = std::chrono::steady_clock::now();
    const LogicalPlane* from[2] = { &job.luma, &job.chroma };
    for (unsigned i = 0; i < 2; i++) {
        const auto& to = job.export_layout[i];
        const unsigned rows = i == 0 ? job.height : (job.height + 1) / 2;
        const unsigned bytes = i == 0 ? job.width : ((job.width + 1) & ~1u);
        if (!logical_plane_fits(*from[i], job.target->size(), rows, bytes)
            || !logical_plane_fits(to, job.export_target->size(), rows, bytes)) {
            throw std::runtime_error("the complete NV12 picture does not fit into its exported buffer");
        }
        for (unsigned row = 0; row < rows; row++) {
            memcpy(job.export_target->data() + to.offset + static_cast<size_t>(row) * to.pitch,
                job.target->data() + from[i]->offset + static_cast<size_t>(row) * from[i]->pitch, bytes);
        }
    }
    debug::add_timing("copy of a picture into its export buffer",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void run_conversion(const ConversionJob& job)
{
    switch (job.conversion) {
    case Conversion::MM21ToNV12: {
        const auto& target = *job.target;

        const auto started = std::chrono::steady_clock::now();
        target.begin_cpu_access();
        mm21::to_nv12(job.source[0].data(), job.source_stride[0], job.source[1].data(), job.source_stride[1],
            target.data() + job.luma.offset, job.luma.pitch, target.data() + job.chroma.offset, job.chroma.pitch,
            job.width, job.height);
        target.end_cpu_access();
        debug::add_timing("MM21 to NV12 conversion",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());

        dump_picture(job);
        if (job.export_target) {
            copy_to_export(job);
        }
        break;
    }
    case Conversion::None:
        break;
    }
}

// Produces an exportable picture with the hardware converter (MDP3), for the case that the pictures converted by the
// CPU are not in DMA-BUFs (there is neither dma-heap nor udmabuf), which are what exporting needs.
struct Mdp3Export {
    int fd; // new descriptor, to be handed out
    size_t size;
    BufferLayout layout;
};

Mdp3Export export_with_mdp3(DriverData* driver_data, VASurfaceID surface_id, const Surface& surface)
{
    if (debug::quirk("nomdp3")) {
        throw std::runtime_error("hardware conversion (MDP3) is switched off");
    }

    // Surfaces don't know about contexts, but contexts know their surfaces.
    Context* owner = nullptr;
    std::optional<unsigned> index;
    {
        std::lock_guard<std::mutex> guard(driver_data->mutex);
        for (const auto& [context_id, context] : driver_data->contexts) {
            if ((index = context->buffer_index(surface_id))) {
                owner = context.get();
                break;
            }
        }
    }
    if (!owner) {
        throw std::runtime_error("the surface doesn't belong to a context");
    }

    std::string error;
    Mdp3Converter* converter = owner->mdp3_converter(error);
    if (!converter) {
        throw std::runtime_error("hardware conversion (MDP3) isn't usable: " + error);
    }

    // The decoded picture, as DMA-BUFs the MDP3 can read.
    const auto& capture = surface.destination_buffer->get();
    std::vector<int> source_fds = capture.export_(O_RDONLY);
    std::vector<size_t> source_sizes;
    for (auto&& plane : capture.mapping()) {
        source_sizes.push_back(plane.size());
    }

    Mdp3Converter::Result result;
    try {
        result = converter->convert(*index, source_fds, source_sizes);
    } catch (const std::exception& e) {
        for (const int fd : source_fds) {
            close(fd);
        }
        owner->disable_mdp3(e.what());
        throw std::runtime_error(std::string("hardware conversion (MDP3) failed: ") + e.what());
    }
    for (const int fd : source_fds) {
        close(fd);
    }

    const int duplicate = fcntl(result.fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        throw std::system_error(errno, std::generic_category(), "Unable to duplicate DMA-BUF descriptor");
    }

    return Mdp3Export { duplicate, result.size,
        BufferLayout {
            { 0, result.uv_offset, result.pitch, 0 },
            { 0, result.size - result.uv_offset, result.pitch, result.uv_offset },
        } };
}

// Exports the converted picture through a buffer of the display driver (DRM): copied there, as our own buffer is
// no DMA-BUF. That needs nothing from the kernel but a display driver, and is what is used if there's no dma-heap.
// From then on that buffer belongs to the surface: every picture decoded into it is copied there as well.
struct DrmExport {
    int fd;
    size_t size;
};

DrmExport export_via_drm(Surface& surface)
{
    if (debug::quirk("nodrm")) {
        throw std::runtime_error("exporting through the display driver is switched off");
    }
    const auto& converted = *surface.converted_buffer;

    surface.export_buffer = std::make_shared<DrmBuffer>(DrmBuffer::allocate(converted.size()));
    surface.export_layout = surface.logical_destination_layout;
    memcpy(surface.export_buffer->data(), converted.data(), converted.size());

    return DrmExport { surface.export_buffer->duplicate_fd(), surface.export_buffer->size() };
}

// A picture to export for a surface nothing has been decoded into (yet). Players ask for that to find out what the
// driver is able to export, and some (Chromium) export all their surfaces once, before decoding anything, expecting
// whatever is decoded into the surface later to show up in what they got. So it is a proper DMA-BUF, kept by the
// surface, and filled with black until there is a picture.
struct PlaceholderExport {
    int fd;
    size_t size;
    BufferLayout layout;
};

PlaceholderExport export_placeholder(Surface& surface)
{
    const unsigned pitch = align_up(surface.width, 64);
    const unsigned rows = align_up(surface.height, 16);
    const auto layout = nv12_layout_with_pitch(pitch, rows);
    const size_t size = layout.back().offset + layout.back().size;

    if (!debug::quirk("nodrm")) {
        try {
            auto buffer = std::make_shared<DrmBuffer>(DrmBuffer::allocate(size));
            memset(buffer->data() + layout[0].offset, 16, layout[0].size);
            memset(buffer->data() + layout[1].offset, 128, layout[1].size);

            surface.export_buffer = buffer;
            surface.export_layout = layout;
            surface.complete_before_sync = true;
            return PlaceholderExport { buffer->duplicate_fd(), buffer->size(), layout };
        } catch (const std::runtime_error&) { }
    }

    try {
        const auto buffer = DmaBuffer::allocate(size, true);
        return PlaceholderExport { buffer.duplicate_fd(), buffer.size(), layout };
    } catch (const std::runtime_error&) { }

    if (debug::quirk("nomdp3")) {
        throw std::runtime_error("no DMA-BUF allocator (dma-heap, udmabuf or display driver) is available");
    }
    const auto allocated = Mdp3Converter::allocate_nv12(align_up(surface.width, 16), align_up(surface.height, 16));
    return PlaceholderExport { allocated.fd, allocated.size,
        BufferLayout {
            { 0, allocated.uv_offset, allocated.pitch, 0 },
            { 0, allocated.size - allocated.uv_offset, allocated.pitch, allocated.uv_offset },
        } };
}

// The memory the application made for a surface (import mode), see VASurfaceAttribExternalBuffers and
// VADRMPRIMESurfaceDescriptor: a single NV12 buffer, which the pictures are put into.
struct ImportedBuffer {
    std::shared_ptr<DmaBuffer> buffer;
    BufferLayout layout;
};

ImportedBuffer import_external_buffer(unsigned memory_type, void* descriptor)
{
    int fd = -1;
    size_t size = 0;
    unsigned planes = 0;
    unsigned pitches[2] = {}, offsets[2] = {};

    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        const auto& prime = *static_cast<const VADRMPRIMESurfaceDescriptor*>(descriptor);
        if (prime.fourcc != VA_FOURCC_NV12) {
            throw std::runtime_error("only NV12 buffers can be imported");
        }

        // The planes (in whichever layers) are expected to be parts of the same buffer, perhaps given as several descriptors.
        int first_object = -1;
        struct stat first_status = {};
        for (unsigned layer = 0; layer < prime.num_layers; layer++) {
            for (unsigned plane = 0; plane < prime.layers[layer].num_planes; plane++) {
                if (planes >= 2) {
                    throw std::runtime_error("the buffer has more than two planes");
                }
                const unsigned object = prime.layers[layer].object_index[plane];
                if (object >= prime.num_objects) {
                    throw std::runtime_error("the buffer describes a plane of an object that doesn't exist");
                }
                struct stat status = {};
                if (fstat(prime.objects[object].fd, &status) < 0) {
                    throw std::runtime_error("the buffer's descriptor is invalid");
                }
                if (first_object < 0) {
                    first_object = object;
                    first_status = status;
                } else if (status.st_ino != first_status.st_ino || status.st_dev != first_status.st_dev) {
                    throw std::runtime_error("the planes of the buffer are in different buffers");
                }
                pitches[planes] = prime.layers[layer].pitch[plane];
                offsets[planes] = prime.layers[layer].offset[plane];
                planes++;
            }
        }
        if (planes != 2 || first_object < 0) {
            throw std::runtime_error("the buffer doesn't have the two planes of NV12");
        }
        const auto modifier = prime.objects[first_object].drm_format_modifier;
        if (modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID) {
            throw std::runtime_error("the buffer has a modifier (not linear) pictures can't be written to");
        }
        fd = prime.objects[first_object].fd;
        size = prime.objects[first_object].size;
    } else {
        const auto& external = *static_cast<const VASurfaceAttribExternalBuffers*>(descriptor);
        if (external.pixel_format != VA_FOURCC_NV12 || external.num_planes != 2) {
            throw std::runtime_error("only NV12 buffers of two planes can be imported");
        }
        if (external.num_buffers != 1 || !external.buffers) {
            throw std::runtime_error("the picture has to be in one buffer");
        }
        fd = static_cast<int>(external.buffers[0]);
        size = external.data_size;
        planes = 2;
        for (unsigned i = 0; i < 2; i++) {
            pitches[i] = external.pitches[i];
            offsets[i] = external.offsets[i];
        }
    }

    ImportedBuffer imported;
    imported.buffer = std::make_shared<DmaBuffer>(DmaBuffer::import(fd, size));
    const size_t available = imported.buffer->size();

    // Luma comes first, chroma after it (that's what NV12 in one buffer is).
    if (pitches[0] == 0 || pitches[1] == 0 || offsets[1] < offsets[0] + pitches[0] || offsets[1] + pitches[1] > available) {
        throw std::runtime_error("the layout of the buffer makes no sense");
    }
    imported.layout = BufferLayout {
        { 0, offsets[1] - offsets[0], pitches[0], offsets[0] },
        { 0, static_cast<unsigned>(available - offsets[1]), pitches[1], offsets[1] },
    };
    return imported;
}

} // namespace

VAStatus createSurfaces2(VADriverContextP context, unsigned int format, unsigned int width, unsigned int height,
    VASurfaceID* surfaces_ids, unsigned int surfaces_count, VASurfaceAttrib* attributes, unsigned int attributes_count)
{
    // TODO ensure dimensions match previous surfaces

    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    // Applications can hand us the memory for the surface (Chromium does): the pictures have to go there.
    unsigned memory_type = 0;
    unsigned usage_hint = VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC;
    void* external_descriptor = nullptr;
    for (unsigned i = 0; i < attributes_count; i++) {
        if (attributes[i].type == VASurfaceAttribMemoryType) {
            memory_type = attributes[i].value.value.i;
        } else if (attributes[i].type == VASurfaceAttribExternalBufferDescriptor) {
            external_descriptor = attributes[i].value.value.p;
        } else if (attributes[i].type == VASurfaceAttribUsageHint
            && attributes[i].value.type == VAGenericValueTypeInteger) {
            usage_hint = static_cast<unsigned>(attributes[i].value.value.i);
        }
    }
    const bool importing = external_descriptor
        && (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME || memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2);
    if (getenv("LIBVA_V4L2_TRACE")) {
        fprintf(stderr, "libva-v4l2: creating %u surfaces of %ux%u, memory type 0x%x, %s\n", surfaces_count, width, height,
            memory_type, importing ? "buffer provided by the application" : "own buffers");
    }

    ImportedBuffer imported;
    if (importing) {
        if (surfaces_count != 1) {
            error_log(context, "Importing a buffer is only possible for one surface at a time.\n");
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        try {
            imported = import_external_buffer(memory_type, external_descriptor);
        } catch (const std::exception& e) {
            error_log(context, "Unable to use the buffer of the application: %s\n", e.what());
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    }

    const bool decoder_surface = std::ranges::any_of(
        driver_data->devices, [&](auto&& d) { return matching_format(d, format) != formats.end(); });
    const bool encoder_surface = format == VA_RT_FORMAT_YUV420
        && std::ranges::any_of(driver_data->devices, [](const auto& d) {
               return H264EncoderContext::supports_nv12_surfaces(d);
           });
    if (!decoder_surface && !encoder_surface) {
        error_log(context, "No matching render target supported by device.\n");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    // The VA-API surface API does not carry a VAConfigID. Prefer its explicit
    // usage hint. With a generic hint, infer encoder-only intent ONLY if
    // at least one encode config and no decode config exist at creation time.
    // A mixed decode/encode process without hints deliberately stays on the
    // safe staging path rather than changing how decoder images are mapped.
    bool has_encoder_config = false;
    bool has_decoder_config = false;
    for (const auto& [id, config] : driver_data->configs) {
        (void)id;
        has_encoder_config |= config.entrypoint == VAEntrypointEncSlice;
        has_decoder_config |= config.entrypoint == VAEntrypointVLD;
    }
    const bool hinted_encoder = (usage_hint & VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER) != 0;
    const bool hinted_decoder = (usage_hint & VA_SURFACE_ATTRIB_USAGE_HINT_DECODER) != 0;
    const bool encoder_candidate = format == VA_RT_FORMAT_YUV420 && !importing
        && !hinted_decoder && (hinted_encoder || (has_encoder_config && !has_decoder_config));
    for (unsigned i = 0; i < surfaces_count; i++) {
        surfaces_ids[i] = smallest_free_key(driver_data->surfaces);
        auto [config, inserted] = driver_data->surfaces.emplace(std::make_pair(surfaces_ids[i],
            Surface {
                .status = VASurfaceReady, .width = width, .height = height, .format = format, .request_fd = -1 }));
        if (!inserted) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        config->second.encoder_upload_candidate = encoder_candidate;
        config->second.usage_hint = usage_hint;
        if (encoder_debug_enabled() && i == 0) {
            std::fprintf(stderr,
                "libva-v4l2: surface purpose: hint=0x%x enc_config=%d dec_config=%d candidate=%d size=%ux%u\n",
                usage_hint, has_encoder_config, has_decoder_config, encoder_candidate, width, height);
        }
        config->second.encoder_input = std::ranges::any_of(driver_data->contexts, [](const auto& entry) {
            return entry.second->encoding;
        });
        if (encoder_debug_enabled() && config->second.encoder_input && i < 4) {
            fprintf(stderr, "libva-v4l2: createSurfaces2 encoder surface=%u size=%ux%u imported=%d\n",
                surfaces_ids[i], width, height, importing);
        }
        if (importing) {
            config->second.imported_buffer = imported.buffer;
            config->second.imported_layout = imported.layout;
            config->second.complete_before_sync = true;
        }
    }

    return VA_STATUS_SUCCESS;
}

void materializeDeferredConversion(Surface& surface)
{
    if (!surface.deferred_encoder_conversion) return;
    surface.deferred_encoder_conversion = false;
    run_conversion(prepare_conversion(surface));
}

void detileDecoderIntoNV12M(const Surface& surface, std::span<uint8_t> y, unsigned y_pitch,
    std::span<uint8_t> uv, unsigned uv_pitch, unsigned width, unsigned height, unsigned group_tiles, bool row_major, unsigned prefetch_tiles)
{
    if (surface.conversion != Conversion::MM21ToNV12 || !surface.destination_buffer)
        throw std::runtime_error("direct detile requires an MM21 decoder capture surface");
    const auto& capture = surface.destination_buffer->get();
    const auto& source_format = capture.owner().capture_format.fmt.pix_mp;
    auto source = capture.mapping();
    check_mm21_mapping(source_format, source);
    if (source_format.pixelformat != V4L2_PIX_FMT_MM21 || width == 0 || height == 0
        || width > surface.width || height > surface.height
        || width > source_format.width || height > source_format.height)
        throw std::runtime_error("invalid MM21 source format or visible crop");
    const unsigned uv_width = (width + 1u) & ~1u;
    const unsigned uv_height = (height + 1u) / 2;
    if (y_pitch < width || uv_pitch < uv_width || y.size() < width || uv.size() < uv_width
        || (height - 1u) > (y.size() - width) / y_pitch
        || (uv_height - 1u) > (uv.size() - uv_width) / uv_pitch)
        throw std::runtime_error("encoder NV12M destination planes are too small");
    mm21::to_nv12(source[0].data(), tiled_stride(source_format, 0),
        source[1].data(), tiled_stride(source_format, 1),
        y.data(), y_pitch, uv.data(), uv_pitch, width, height, group_tiles, row_major, prefetch_tiles);
}

void startConversion(Surface& surface)
{
    if (surface.conversion == Conversion::None) {
        return;
    }
    waitForConversion(surface); // (there is nothing to wait for unless a picture was never synchronised)
    // Restricted to unexported MM21 decoder surfaces. Other users of these surfaces
    // must not enable this transcode-only opt-in: the linear VA image is deferred.
    const char* direct = std::getenv("LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT");
    if (direct && std::strcmp(direct, "1") == 0 && surface.conversion == Conversion::MM21ToNV12
        && !surface.complete_before_sync && !surface.imported_buffer && !surface.export_buffer
        && surface.destination_buffer) {
        surface.deferred_encoder_conversion = true;
        return;
    }
    surface.deferred_encoder_conversion = false;

    const auto job = std::make_shared<ConversionJob>(prepare_conversion(surface));
    try {
        surface.conversion_result = std::async(std::launch::async, [job] { run_conversion(*job); }).share();
    } catch (const std::system_error&) {
        run_conversion(*job); // no thread to be had: do it right now
    }

    // Surfaces the application can look at without synchronising (their memory is its own, or was exported before
    // anything was decoded) must not show a picture that isn't there yet.
    if (surface.complete_before_sync) {
        waitForConversion(surface);
    }
}

void waitForConversion(Surface& surface)
{
    if (!surface.conversion_result.valid()) {
        return;
    }
    const auto result = std::move(surface.conversion_result);
    surface.conversion_result = {};
    result.get(); // (rethrows what went wrong in the thread)
}

void releaseSurfaceBuffers(Surface& surface)
{
    try {
        waitForConversion(surface);
    } catch (std::exception&) { }
    surface.pending_sync = false;
    if (!surface.queued) {
        return;
    }
    // Decoding is synchronous (vaEndPicture waits for the request), so these are complete and dequeue at once.
    try {
        surface.source_buffer->get().dequeue();
    } catch (std::exception&) { }
    try {
        surface.destination_buffer->get().dequeue();
    } catch (std::exception&) { }
    surface.queued = false;
    surface.status = VASurfaceReady;
}

CaptureSetup setupCapture(const Context& context, const Surface& reference, unsigned count)
{
    const auto matching = matching_format(context.device, reference.format);
    if (matching == formats.end()) {
        throw std::runtime_error("Device offers no capture format matching the requested render target");
    }
    const auto& v4l2_format = matching->v4l2;

    context.device.set_format(context.device.capture_buf_type, v4l2_format.format, reference.width, reference.height);

    const v4l2_pix_format_mplane& driver_format = context.device.capture_format.fmt.pix_mp;
    if (driver_format.width < reference.width || driver_format.height < reference.height) {
        throw ResolutionNotSupported("V4L2 returned CAPTURE format " + std::to_string(driver_format.width) + "x"
            + std::to_string(driver_format.height) + " for a VA surface of " + std::to_string(reference.width)
            + "x" + std::to_string(reference.height));
    }

    // If the CPU has to read the decoded pictures, cacheable buffers are a lot faster than the default ones.
    const bool convert = v4l2_format.conversion != Conversion::None;
    const bool cpu_cached = convert && !debug::quirk("nocache");
    const unsigned granted = context.device.request_buffers(context.device.capture_buf_type, count, cpu_cached);

    if (const char* dump = getenv("LIBVA_V4L2_DUMP"); dump && *dump) {
        fprintf(stderr, "libva-v4l2: capture format at setup: %s, %u buffers; output format: %s\n",
            context.device.describe_format(context.device.capture_buf_type).c_str(), granted,
            context.device.describe_format(context.device.output_buf_type).c_str());
    }

    return CaptureSetup { &*matching, convert ? converted_layout(driver_format) : BufferLayout(), granted, cpu_cached };
}

void bindSurface(Surface& surface, const Context& context, const CaptureSetup& setup, unsigned index)
{
    surface.encoder_input = false;
    const auto& [format, derive_layout, conversion] = setup.format->v4l2;
    const v4l2_pix_format_mplane& driver_format = context.device.capture_format.fmt.pix_mp;

    try {
        waitForConversion(surface);
    } catch (std::exception&) { }

    surface.device = context.device_owner;
    surface.destination_buffer = std::cref(context.device.buffer(context.device.capture_buf_type, index));
    surface.logical_destination_layout.clear();
    surface.conversion = conversion;
    surface.converted_buffer.reset();
    surface.deferred_encoder_conversion = false;

    if (surface.imported_buffer && conversion == Conversion::None) {
        throw std::runtime_error("buffers of the application can only be used for formats that need converting");
    }

    if (conversion != Conversion::None) {
        if (conversion == Conversion::MM21ToNV12) {
            check_mm21_mapping(driver_format, surface.destination_buffer->get().mapping());
        }
        if (surface.imported_buffer) {
            // Import mode: the application has its own memory for the picture, and looks at that.
            surface.logical_destination_layout = surface.imported_layout;
            surface.converted_buffer = surface.imported_buffer;
        } else {
            surface.logical_destination_layout = setup.linear_layout;
            surface.converted_buffer = std::make_shared<DmaBuffer>(
                DmaBuffer::allocate(setup.linear_layout.back().offset + setup.linear_layout.back().size));
        }
    } else if (derive_layout) { // (logical) single plane
        surface.logical_destination_layout = derive_layout(driver_format.width, driver_format.height);
    } else {
        for (unsigned j = 0; j < driver_format.num_planes; j += 1) {
            // Each V4L2 memory plane is mapped separately. Its logical offset therefore starts at zero; only
            // single-memory-plane formats (where derive_layout is used above) have contiguous plane offsets.
            surface.logical_destination_layout.push_back({
                j,
                driver_format.plane_fmt[j].sizeimage,
                driver_format.plane_fmt[j].bytesperline,
                0,
            });
        }
    }
}

CaptureSetup createSurfacesDeferred(DriverData* driver_data, const Context& context, std::span<VASurfaceID> surface_ids)
{
    if (surface_ids.size() < 1) {
        throw std::invalid_argument("No surfaces to be created");
    }

    auto setup = setupCapture(context, driver_data->surfaces.at(surface_ids[0]), surface_ids.size());
    for (unsigned i = 0; i < surface_ids.size(); i++) {
        bindSurface(driver_data->surfaces.at(surface_ids[i]), context, setup, i);
    }
    return setup;
}

VAStatus createSurfaces(
    VADriverContextP context, int width, int height, int format, int surfaces_count, VASurfaceID* surfaces_ids)
{
    return createSurfaces2(context, format, width, height, surfaces_ids, surfaces_count, NULL, 0);
}

VAStatus destroySurfaces(VADriverContextP context, VASurfaceID* surfaces_ids, int surfaces_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    for (int i = 0; i < surfaces_count; i++) {
        if (!driver_data->surfaces.contains(surfaces_ids[i])) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        auto& surface = driver_data->surfaces.at(surfaces_ids[i]);

        releaseSurfaceBuffers(surface);

        if (surface.request_fd > 0)
            close(surface.request_fd);

        driver_data->surfaces.erase(surfaces_ids[i]);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus syncSurface(VADriverContextP context, VASurfaceID surface_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    if (surface.status != VASurfaceRendering) {
        return VA_STATUS_SUCCESS;
    }

    // Normally the buffers have been dequeued when the picture was decoded. Only if there's no request to wait for
    // (and so, no way to know when the decoder is done) it's left to here.
    if (surface.queued) {
        try {
            surface.source_buffer->get().dequeue();
            surface.destination_buffer->get().dequeue();
        } catch (std::runtime_error& e) {
            error_log(context, "Failed to dequeue buffer: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        surface.queued = false;
        surface.pending_sync = true;

        try {
            startConversion(surface);
        } catch (std::exception& e) {
            error_log(context, "Failed to convert decoded picture: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (!surface.pending_sync) {
        return VA_STATUS_SUCCESS; // still in the middle of being decoded
    }
    surface.pending_sync = false;
    surface.status = VASurfaceDisplaying;

    // The conversion of formats the application can't use as they are was started when decoding finished, in the
    // background; this is where it's needed.
    try {
        waitForConversion(surface);
    } catch (std::exception& e) {
        error_log(context, "Failed to convert decoded picture: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceAttributes(
    VADriverContextP context, VAConfigID config, VASurfaceAttrib* attributes, unsigned int* attributes_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    const auto found = driver_data->configs.find(config);
    if (found == driver_data->configs.end()) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    // Do not invent 2048x2048 limits when the V4L2 device cannot enumerate them. In that case omit the optional
    // size attributes, matching getConfigAttributes() which reports MaxPictureWidth/Height as NOT_SUPPORTED.
    auto limits = frame_size_limits(driver_data, found->second.profile);
    if (found->second.entrypoint == VAEntrypointEncSlice) {
        limits.reset();
        for (const auto& device : driver_data->devices) {
            if (H264EncoderContext::supported_profiles(device).contains(found->second.profile)) {
                limits = device.frame_size_limits(V4L2_PIX_FMT_NV12M);
                break;
            }
        }
    }
    VASurfaceAttrib attributes_list[Config::max_attributes] = {};
    unsigned int i = 0;

    attributes_list[i].type = VASurfaceAttribPixelFormat;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;
    attributes_list[i].value.value.i = VA_FOURCC_NV12;
    i++;

    if (limits) {
        attributes_list[i].type = VASurfaceAttribMinWidth;
        attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
        attributes_list[i].value.type = VAGenericValueTypeInteger;
        attributes_list[i].value.value.i = limits->min_width;
        i++;

        attributes_list[i].type = VASurfaceAttribMaxWidth;
        attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
        attributes_list[i].value.type = VAGenericValueTypeInteger;
        attributes_list[i].value.value.i = limits->max_width;
        i++;

        attributes_list[i].type = VASurfaceAttribMinHeight;
        attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
        attributes_list[i].value.type = VAGenericValueTypeInteger;
        attributes_list[i].value.value.i = limits->min_height;
        i++;

        attributes_list[i].type = VASurfaceAttribMaxHeight;
        attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
        attributes_list[i].value.type = VAGenericValueTypeInteger;
        attributes_list[i].value.value.i = limits->max_height;
        i++;
    }

    attributes_list[i].type = VASurfaceAttribMemoryType;
    attributes_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attributes_list[i].value.type = VAGenericValueTypeInteger;
    attributes_list[i].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    i++;

    if (attributes != NULL) {
        memcpy(attributes, attributes_list, i * sizeof(*attributes));
    }
    *attributes_count = i;
    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceStatus(VADriverContextP context, VASurfaceID surface_id, VASurfaceStatus* status)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    *status = driver_data->surfaces.at(surface_id).status;

    return VA_STATUS_SUCCESS;
}

VAStatus putSurface(VADriverContextP context, VASurfaceID surface_id, void* draw, short src_x, short src_y,
    unsigned short src_width, unsigned short src_height, short dst_x, short dst_y, unsigned short dst_width,
    unsigned short dst_height, VARectangle* cliprects, unsigned int cliprects_count, unsigned int flags)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus lockSurface(VADriverContextP context, VASurfaceID surface_id, unsigned int* fourcc, unsigned int* luma_stride,
    unsigned int* chroma_u_stride, unsigned int* chroma_v_stride, unsigned int* luma_offset,
    unsigned int* chroma_u_offset, unsigned int* chroma_v_offset, unsigned int* buffer_name, void** buffer)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus unlockSurface(VADriverContextP context, VASurfaceID surface_id)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus exportSurfaceHandle(
    VADriverContextP context, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void* descriptor)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    auto surface_descriptor = static_cast<VADRMPRIMESurfaceDescriptor*>(descriptor);

    if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    // A surface can already have writable linear backing for encode, even though it has no decoder CAPTURE buffer.
    const bool decoded = surface.destination_buffer.has_value() || surface.converted_buffer || surface.export_buffer;

    // Converted pictures only exist once the surface has been synchronised, so make sure that has happened.
    if (decoded && surface.status == VASurfaceRendering) {
        if (const auto status = syncSurface(context, surface_id); status != VA_STATUS_SUCCESS) {
            return status;
        }
    }

    std::vector<int> export_fds;
    std::vector<size_t> object_sizes;
    BufferLayout layout; // of the picture in the (first) object
    bool nv12 = true; // false if it is whatever the decoder produces
    try {
        if (surface.encoder_drm_planes.size() == 2) {
            // Two linear NV12M PRIME objects, directly usable by VENC.
            for (const auto& plane : surface.encoder_drm_planes) {
                export_fds.push_back(plane->duplicate_fd());
                object_sizes.push_back(plane->size());
            }
            layout = surface.logical_destination_layout;
        } else if (surface.imported_buffer) {
            // The application's own buffer, which the pictures are put into.
            export_fds.push_back(surface.imported_buffer->duplicate_fd());
            object_sizes.push_back(surface.imported_buffer->size());
            layout = surface.imported_layout;
        } else if (surface.export_buffer) {
            // Exported before: the same buffer again (it is kept up to date).
            export_fds.push_back(surface.export_buffer->duplicate_fd());
            object_sizes.push_back(surface.export_buffer->size());
            layout = surface.export_layout;
        } else if (!decoded) {
            // Nothing decoded into it (yet): there's only a placeholder to hand out.
            auto placeholder = export_placeholder(surface);
            export_fds.push_back(placeholder.fd);
            object_sizes.push_back(placeholder.size);
            layout = std::move(placeholder.layout);
        } else if (surface.converted_buffer && surface.converted_buffer->exportable()) {
            // Converted pictures live in our own linear buffer, the V4L2 one is in a device specific layout.
            export_fds.push_back(surface.converted_buffer->duplicate_fd());
            object_sizes.push_back(surface.converted_buffer->size());
            layout = surface.logical_destination_layout;
        } else if (surface.converted_buffer) {
            // Our buffer is no DMA-BUF (the kernel has neither dma-heap nor udmabuf). Copy the picture to a buffer of
            // the display driver, or if that isn't possible, have the hardware converter produce an exportable one.
            try {
                const auto exported = export_via_drm(surface);
                export_fds.push_back(exported.fd);
                object_sizes.push_back(exported.size);
                layout = surface.export_layout;
            } catch (std::runtime_error& drm_error) {
                try {
                    auto exported = export_with_mdp3(driver_data, surface_id, surface);
                    export_fds.push_back(exported.fd);
                    object_sizes.push_back(exported.size);
                    layout = std::move(exported.layout);
                } catch (std::runtime_error& mdp3_error) {
                    throw std::runtime_error(std::string("no DMA-BUF to export: ") + drm_error.what() + "; "
                        + mdp3_error.what() + "; (or use a kernel with CONFIG_DMABUF_HEAPS_SYSTEM or CONFIG_UDMABUF)");
                }
            }
        } else {
            export_fds = surface.destination_buffer->get().export_(O_RDONLY);
            for (auto&& plane : surface.destination_buffer->get().mapping()) {
                object_sizes.push_back(plane.size());
            }
            layout = surface.logical_destination_layout;
            nv12 = false;
        }
    } catch (std::runtime_error& e) {
        error_log(context, "Failed to export buffer: %s\n", e.what());
        for (const int fd : export_fds) {
            close(fd);
        }
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    surface_descriptor->fourcc = VA_FOURCC_NV12;
    surface_descriptor->width = surface.width;
    surface_descriptor->height = surface.height;
    surface_descriptor->num_objects = export_fds.size();

    const auto& format_spec = lookup_format(
        nv12 ? V4L2_PIX_FMT_NV12 : surface.destination_buffer->get().owner().capture_format.fmt.pix_mp.pixelformat);
    for (unsigned i = 0; i < export_fds.size(); i += 1) {
        surface_descriptor->objects[i].drm_format_modifier = format_spec.drm.modifier;
        surface_descriptor->objects[i].fd = export_fds[i];
        surface_descriptor->objects[i].size = object_sizes[i];
    }

    // The caller says whether it wants one layer for the whole picture (e.g. NV12), or one per plane (R8 for luma,
    // GR88 for the interleaved chroma). libva requires one of the two to be honoured, and consumers like mpv and
    // ffmpeg usually ask for separate layers.
    const bool separate_layers = (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) && layout.size() == 2
        && format_spec.drm.format == DRM_FORMAT_NV12;

    if (separate_layers) {
        surface_descriptor->num_layers = 2;
        for (unsigned i = 0; i < 2; i++) {
            surface_descriptor->layers[i].drm_format = (i == 0) ? DRM_FORMAT_R8 : DRM_FORMAT_GR88;
            surface_descriptor->layers[i].num_planes = 1;
            surface_descriptor->layers[i].object_index[0] = layout[i].physical_plane_index;
            surface_descriptor->layers[i].pitch[0] = layout[i].pitch;
            surface_descriptor->layers[i].offset[0] = layout[i].offset;
        }
        return VA_STATUS_SUCCESS;
    }

    surface_descriptor->num_layers = 1;

    surface_descriptor->layers[0].drm_format = format_spec.drm.format;
    surface_descriptor->layers[0].num_planes = layout.size();

    for (unsigned i = 0; i < surface_descriptor->layers[0].num_planes; i++) {
        surface_descriptor->layers[0].object_index[i] = layout[i].physical_plane_index;
        surface_descriptor->layers[0].pitch[i] = layout[i].pitch;
        surface_descriptor->layers[0].offset[i] = layout[i].offset;
    }

    return VA_STATUS_SUCCESS;
}
