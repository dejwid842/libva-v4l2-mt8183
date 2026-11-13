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

#include "image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <va/va.h>

extern "C" {
#include <linux/videodev2.h>
#include <unistd.h>
}

#include "buffer.h"
#include "debug.h"
#include "driver.h"
#include "format.h"
#include "surface.h"
#include "encoder_debug_flag.h"
#include "utils.h"
#include "v4l2.h"

namespace {

// Limit tracing to the first few events to avoid flooding FFmpeg logs.
bool prime_trace_event()
{
    if (!encoder_debug_enabled()) return false;
    static std::atomic<unsigned> count { 0 };
    return count.fetch_add(1, std::memory_order_relaxed) < 96;
}


// Import-side cache synchronization is essential on ARM. The DRM buffer's
// CPU mapping is write-combined and V4L2 sees it through another device.
class EncoderPlaneCpuAccess {
public:
    EncoderPlaneCpuAccess(const std::vector<std::shared_ptr<DrmBuffer>>& planes, bool write)
        : write_(write)
    {
        fds_.reserve(planes.size());
        try {
            for (const auto& plane : planes) {
                int fd = plane->duplicate_fd();
                dma_buf_sync sync = {};
                sync.flags = DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
                if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
                    int error = errno;
                    close(fd);
                    throw std::system_error(error, std::generic_category(), "DMA-BUF CPU sync start");
                }
                fds_.push_back(fd);
            }
        } catch (...) {
            const int cleanup_error = finish_impl();
            if (cleanup_error)
                std::fprintf(stderr, "libva-v4l2: encoder DMA-BUF sync END during failed START: %s\n", std::strerror(cleanup_error));
            throw;
        }
    }
    EncoderPlaneCpuAccess(const EncoderPlaneCpuAccess&) = delete;
    EncoderPlaneCpuAccess& operator=(const EncoderPlaneCpuAccess&) = delete;
    ~EncoderPlaneCpuAccess() noexcept
    {
        const int error = finish_impl();
        if (error)
            std::fprintf(stderr, "libva-v4l2: encoder DMA-BUF CPU sync END during cleanup failed: %s\n", std::strerror(error));
    }
    void finish()
    {
        const int error = finish_impl();
        if (error)
            throw std::system_error(error, std::generic_category(), "ending encoder DMA-BUF CPU access");
    }
private:
    int finish_impl() noexcept
    {
        int first_error = 0;
        for (int fd : fds_) {
            dma_buf_sync sync = {};
            sync.flags = DMA_BUF_SYNC_END | (write_ ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
            if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && !first_error)
                first_error = errno;
            close(fd);
        }
        fds_.clear();
        return first_error;
    }
    bool write_;
    std::vector<int> fds_;
};

void maybe_allocate_encoder_prime_surface(Surface& surface)
{
    if (surface.destination_buffer || surface.imported_buffer || surface.export_buffer
        || surface.converted_buffer || !surface.encoder_drm_planes.empty()) {
        if (prime_trace_event()) {
            std::fprintf(stderr,
                "libva-v4l2: PRIME allocation skipped: destination=%d imported=%d export=%d converted=%d planes=%zu\n",
                static_cast<bool>(surface.destination_buffer), static_cast<bool>(surface.imported_buffer),
                static_cast<bool>(surface.export_buffer), static_cast<bool>(surface.converted_buffer),
                surface.encoder_drm_planes.size());
        }
        return;
    }
    const auto* direct = std::getenv("LIBVA_V4L2_ENC_DIRECT");
    const auto* dmabuf = std::getenv("LIBVA_V4L2_ENC_DMABUF");
    if (!direct || std::strcmp(direct, "1") != 0 || !dmabuf || std::strcmp(dmabuf, "1") != 0) {
        return;
    }
    // MT8183 NV12M pads the coded height to 32 lines and allocates 32
    // additional Y rows / 16 additional UV rows in the driver.
    const size_t pitch = (static_cast<size_t>(surface.width) + 15) & ~size_t(15);
    const size_t coded_height = (static_cast<size_t>(surface.height) + 31) & ~size_t(31);
    const size_t sizes[2] = { pitch * (coded_height + 32), pitch * (coded_height / 2 + 16) };
    std::vector<std::shared_ptr<DrmBuffer>> allocated;
    allocated.reserve(2);
    for (unsigned i = 0; i < 2; ++i) {
        allocated.push_back(std::make_shared<DrmBuffer>(DrmBuffer::allocate(sizes[i])));
    }
    {
        EncoderPlaneCpuAccess guard(allocated, true);
        std::memset(allocated[0]->data(), 0x10, allocated[0]->size());
        std::memset(allocated[1]->data(), 0x80, allocated[1]->size());
        guard.finish();
    }
    surface.logical_destination_layout = {
        { 0, static_cast<unsigned>(allocated[0]->size()), static_cast<unsigned>(pitch), 0 },
        { 1, static_cast<unsigned>(allocated[1]->size()), static_cast<unsigned>(pitch), 0 }
    };
    surface.encoder_drm_planes = std::move(allocated);
    if (encoder_debug_enabled()) {
        std::fprintf(stderr, "libva-v4l2: VENC direct NV12M PRIME surface allocated %ux%u\n",
            surface.width, surface.height);
    }
}

bool image_plane_fits(const LogicalPlane& plane, size_t mapping_size, unsigned rows, unsigned row_bytes)
{
    if (rows == 0 || row_bytes == 0 || plane.pitch < row_bytes || plane.offset > mapping_size
        || plane.size > mapping_size - plane.offset || plane.size < row_bytes) {
        return false;
    }
    return static_cast<size_t>(rows - 1) <= (plane.size - row_bytes) / plane.pitch;
}

bool image_destination_fits(size_t buffer_size, unsigned offset, unsigned pitch, unsigned rows, unsigned row_bytes)
{
    if (rows == 0 || row_bytes == 0 || pitch < row_bytes || offset > buffer_size) {
        return false;
    }
    const size_t available = buffer_size - offset;
    if (row_bytes > available) {
        return false;
    }
    return static_cast<size_t>(rows - 1) <= (available - row_bytes) / pitch;
}

VAStatus copy_surface_to_image(DriverData* driver_data, const Surface& surface, VAImage* image)
{
    if (!driver_data->buffers.contains(image->buf)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    auto& buffer = driver_data->buffers.at(image->buf);

    const auto& layout = surface.logical_destination_layout;
    if (image->num_planes != layout.size() || image->num_planes > 2 || !buffer.data) {
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }
    if (image->width > surface.width || image->height > surface.height) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    // For converted formats (e.g. MM21) this is the linear buffer, not the V4L2 one.
    const auto mapping = surface.picture_mapping();
    if (mapping.empty()) {
        return VA_STATUS_ERROR_INVALID_SURFACE; // nothing was ever decoded into this surface
    }
    const auto started = std::chrono::steady_clock::now();

    for (unsigned i = 0; i < layout.size(); i++) {
        const auto& plane = layout[i];
        if (plane.physical_plane_index >= mapping.size()) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        // NV12 chroma rows contain interleaved Cb/Cr bytes, so their active row length is rounded up to even.
        const unsigned rows = i == 0 ? image->height : (image->height + 1) / 2;
        const unsigned row_bytes = i == 0 ? image->width : ((image->width + 1) & ~1u);
        if (!image_plane_fits(plane, mapping[plane.physical_plane_index].size(), rows, row_bytes)
            || !image_destination_fits(buffer.size, image->offsets[i], image->pitches[i], rows, row_bytes)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        const uint8_t* source = mapping[plane.physical_plane_index].data() + plane.offset;
        uint8_t* dest = buffer.data.get() + image->offsets[i];

        // Source and image pitches differ whenever the decoder aligns its buffers: copy every active row in full.
        for (unsigned row = 0; row < rows; row++) {
            std::copy_n(source + static_cast<size_t>(row) * plane.pitch, row_bytes,
                dest + static_cast<size_t>(row) * image->pitches[i]);
        }
    }

    debug::add_timing("copy of a picture into a VAImage",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());

    return VA_STATUS_SUCCESS;
}

} // namespace

VAStatus createImage(VADriverContextP context, VAImageFormat* format, int width, int height, VAImage* image)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    memset(image, 0, sizeof(*image));
    image->format = *format;
    image->width = width;
    image->height = height;

    BufferLayout (*derive_layout)(unsigned, unsigned) = nullptr;
    try {
        derive_layout = lookup_format(format->fourcc).v4l2.derive_layout;
    } catch (std::invalid_argument& e) { // TODO decouple planarity from format layout
        error_log(context, "Image format not specified\n");
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }
    if (!derive_layout) {
        error_log(context, "Image format not specified\n");
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }

    const auto layout = derive_layout(width, height);

    image->num_planes = layout.size();
    for (unsigned i = 0; i < image->num_planes; i += 1) {
        image->data_size += layout[i].size;
        image->pitches[i] = layout[i].pitch;
        image->offsets[i] = layout[i].offset;
    }

    VAStatus status = createBuffer(context, 0, VAImageBufferType, image->data_size, 1, NULL, &image->buf);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    image->image_id = smallest_free_key(driver_data->images);
    auto [image_it, inserted] = driver_data->images.emplace(std::make_pair(image->image_id, *image));
    if (!inserted) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus destroyImage(VADriverContextP context, VAImageID image_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->images.contains(image_id)) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    auto& image = driver_data->images.at(image_id);

    VAStatus status = destroyBuffer(context, image.buf);
    if (status != VA_STATUS_SUCCESS) {
        return status;
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    if (!driver_data->images.erase(image_id)) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus deriveImage(VADriverContextP context, VASurfaceID surface_id, VAImage* image)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    VAImageFormat format;
    VAStatus status;

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    // FFmpeg probes vaDeriveImage before it creates an encoder configuration.
    // If that generic probe succeeded, FFmpeg keeps using deriveImage on the
    // later surfaces of the same hwupload pool. Disabling deriveImage only
    // after VAConfig creation causes an EIO on the second upload. Honour an
    // explicit ENCODER hint, or the diagnostic override, but otherwise keep
    // the first generic upload decision stable.
    const char* force = std::getenv("LIBVA_V4L2_ENC_FORCE_PUTIMAGE");
    const char* direct = std::getenv("LIBVA_V4L2_ENC_DIRECT");
    const char* dmabuf = std::getenv("LIBVA_V4L2_ENC_DMABUF");
    const bool direct_enabled = direct && std::strcmp(direct, "1") == 0
        && dmabuf && std::strcmp(dmabuf, "1") == 0;
    const bool force_putimage = force && std::strcmp(force, "1") == 0;
    const bool explicit_encoder_hint =
        (surface.usage_hint & VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER) != 0;
    const bool generic_derive_chosen =
        driver_data->generic_derive_succeeded.load(std::memory_order_relaxed);
    const bool select_putimage = direct_enabled
        && (force_putimage || (surface.encoder_upload_candidate
            && (explicit_encoder_hint || !generic_derive_chosen)));
    if (select_putimage) {
        if (prime_trace_event()) {
            std::fprintf(stderr,
                "libva-v4l2: vaDeriveImage select PutImage surface=%u candidate=%d force=%d explicit_encoder=%d generic_derive=%d\n",
                surface_id, surface.encoder_upload_candidate, force_putimage,
                explicit_encoder_hint, generic_derive_chosen);
        }
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (direct_enabled && surface.encoder_upload_candidate && generic_derive_chosen
        && !explicit_encoder_hint && !force_putimage && prime_trace_event()) {
        std::fprintf(stderr,
            "libva-v4l2: preserving generic vaDeriveImage upload decision for surface=%u (encoder DMA-BUF staging fallback)\n",
            surface_id);
    }

    if (prime_trace_event()) {
        std::fprintf(stderr, "libva-v4l2: vaDeriveImage surface=%u %ux%u encoder=%d converted=%d prime_planes=%zu\n",
            surface_id, surface.width, surface.height, surface.encoder_input,
            static_cast<bool>(surface.converted_buffer), surface.encoder_drm_planes.size());
    }

    memset(&format, 0, sizeof(format));
    format.fourcc = VA_FOURCC_NV12;
    format.bits_per_pixel = 12;

    // Encoder input surfaces need stable, directly writable NV12 backing memory. Keep the historical
    // placeholder-image behaviour for decoder surfaces that have not been bound to a V4L2 queue yet.
    if (!surface.destination_buffer && !surface.encoder_input) {
        status = createImage(context, &format, surface.width, surface.height, image);
        if (status != VA_STATUS_SUCCESS) {
            return status;
        }
        driver_data->buffers.at(image->buf).derived_surface_id = surface_id;
        if (surface.usage_hint == VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC) {
            driver_data->generic_derive_succeeded.store(true, std::memory_order_relaxed);
        }
        return VA_STATUS_SUCCESS;
    }

    if (!surface.destination_buffer && surface.encoder_input) {
        memset(image, 0, sizeof(*image));
        image->format = format;
        image->width = surface.width;
        image->height = surface.height;
        try {
            if (surface.imported_buffer) {
                surface.converted_buffer = surface.imported_buffer;
                surface.logical_destination_layout = surface.imported_layout;
            } else if (!surface.converted_buffer && !surface.export_buffer) {
                if (prime_trace_event()) {
                    std::fprintf(stderr, "libva-v4l2: vaDeriveImage allocating single-buffer NV12 surface (blocks direct NV12M)\n");
                }
                surface.converted_buffer = std::make_shared<DmaBuffer>(
                    DmaBuffer::allocate(static_cast<size_t>(surface.width) * surface.height * 3 / 2));
                surface.logical_destination_layout = nv12_layout_with_pitch(surface.width, surface.height);
            }

            if (surface.export_buffer && !surface.converted_buffer) {
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
            }

            const auto& layout = surface.logical_destination_layout;
            if (!surface.converted_buffer || layout.size() != 2
                || !image_plane_fits(layout[0], surface.converted_buffer->size(), surface.height, surface.width)
                || !image_plane_fits(layout[1], surface.converted_buffer->size(), (surface.height + 1) / 2,
                    (surface.width + 1) & ~1u)) {
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }
            image->num_planes = layout.size();
            for (unsigned i = 0; i < image->num_planes; i++) {
                image->pitches[i] = layout[i].pitch;
                image->offsets[i] = layout[i].offset;
            }
            image->data_size = layout.back().offset + layout.back().size;
            status = createExternalBuffer(context, VAImageBufferType, image->data_size, surface.converted_buffer->data(),
                surface.converted_buffer, surface_id, &image->buf, surface.converted_buffer);
            if (status != VA_STATUS_SUCCESS) {
                return status;
            }
            std::lock_guard<std::mutex> guard(driver_data->mutex);
            image->image_id = smallest_free_key(driver_data->images);
            if (!driver_data->images.emplace(std::make_pair(image->image_id, *image)).second) {
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
            return VA_STATUS_SUCCESS;
        } catch (const std::exception& e) {
            error_log(context, "Unable to allocate input surface image: %s\n", e.what());
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    if (surface.status == VASurfaceRendering) {
        status = syncSurface(context, surface_id);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }

    if (surface.converted_buffer && surface.logical_destination_layout.size() == 2) {
        // That is what deriving is for: the image shows the surface's own memory, there's nothing to copy (and
        // allocating, zeroing and filling a new buffer for every picture is expensive).
        const auto& layout = surface.logical_destination_layout;
        const unsigned chroma_width = (surface.width + 1) & ~1u;
        if (layout.size() != 2
            || !image_plane_fits(layout[0], surface.converted_buffer->size(), surface.height, surface.width)
            || !image_plane_fits(layout[1], surface.converted_buffer->size(), (surface.height + 1) / 2,
                chroma_width)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        memset(image, 0, sizeof(*image));
        image->format = format;
        image->width = surface.width;
        image->height = surface.height;
        image->num_planes = layout.size();
        for (unsigned i = 0; i < image->num_planes; i++) {
            image->pitches[i] = layout[i].pitch;
            image->offsets[i] = layout[i].offset;
        }
        image->data_size = layout.back().offset + layout.back().size;

        status = createExternalBuffer(context, VAImageBufferType, image->data_size, surface.converted_buffer->data(),
            surface.converted_buffer, surface_id, &image->buf, surface.converted_buffer);
        if (status != VA_STATUS_SUCCESS)
            return status;

        std::lock_guard<std::mutex> guard(driver_data->mutex);
        image->image_id = smallest_free_key(driver_data->images);
        if (!driver_data->images.emplace(std::make_pair(image->image_id, *image)).second) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surface.status = VASurfaceReady;
        return VA_STATUS_SUCCESS;
    }

    status = createImage(context, &format, surface.width, surface.height, image);
    if (status != VA_STATUS_SUCCESS)
        return status;

    status = copy_surface_to_image(driver_data, surface, image);
    if (status != VA_STATUS_SUCCESS) {
        destroyImage(context, image->image_id);
        return status;
    }

    surface.status = VASurfaceReady;

    if (!driver_data->buffers.contains(image->buf)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    driver_data->buffers.at(image->buf).derived_surface_id = surface_id;

    return VA_STATUS_SUCCESS;
}

VAStatus queryImageFormats(VADriverContextP context, VAImageFormat* formats, int* formats_count)
{
    formats[0].fourcc = VA_FOURCC_NV12;
    *formats_count = 1;

    return VA_STATUS_SUCCESS;
}

VAStatus setImagePalette(VADriverContextP context, VAImageID image_id, unsigned char* palette)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus getImage(VADriverContextP context, VASurfaceID surface_id, int x, int y, unsigned int width,
    unsigned int height, VAImageID image_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (!driver_data->images.contains(image_id)) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    auto& image = driver_data->images.at(image_id);

    if (x != 0 || y != 0 || width != image.width || height != image.height)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    if (driver_data->surfaces.at(surface_id).status == VASurfaceRendering) {
        if (const auto status = syncSurface(context, surface_id); status != VA_STATUS_SUCCESS) {
            return status;
        }
    }

    return copy_surface_to_image(driver_data, driver_data->surfaces.at(surface_id), &image);
}

VAStatus putImage(VADriverContextP context, VASurfaceID surface_id, VAImageID image_id, int src_x, int src_y,
    unsigned int src_width, unsigned int src_height, int dst_x, int dst_y, unsigned int dst_width,
    unsigned int dst_height)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    const auto surface_it = driver_data->surfaces.find(surface_id);
    const auto image_it = driver_data->images.find(image_id);
    if (surface_it == driver_data->surfaces.end()) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (image_it == driver_data->images.end()) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    auto& surface = surface_it->second;
    const auto& image = image_it->second;
    if (prime_trace_event()) {
        std::fprintf(stderr,
            "libva-v4l2: vaPutImage surface=%u image=%u src=%ux%u dst=%ux%u converted=%d prime_planes=%zu imported=%d\n",
            surface_id, image_id, src_width, src_height, dst_width, dst_height,
            static_cast<bool>(surface.converted_buffer), surface.encoder_drm_planes.size(),
            static_cast<bool>(surface.imported_buffer));
    }
    // VA surfaces may be uploaded before being attached to a VA encode
    // context. Only accept pre-context uploads for surfaces classified at
    // creation, or with the explicit legacy FORCE_PUTIMAGE override.
    if (!surface.encoder_input) {
        const char* force = std::getenv("LIBVA_V4L2_ENC_FORCE_PUTIMAGE");
        const char* direct = std::getenv("LIBVA_V4L2_ENC_DIRECT");
        const char* dmabuf = std::getenv("LIBVA_V4L2_ENC_DMABUF");
        const bool direct_enabled = direct && std::strcmp(direct, "1") == 0
            && dmabuf && std::strcmp(dmabuf, "1") == 0;
        const bool force_putimage = force && std::strcmp(force, "1") == 0;
        if (!direct_enabled || (!surface.encoder_upload_candidate && !force_putimage)) {
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        }
        if (prime_trace_event()) {
            std::fprintf(stderr,
                "libva-v4l2: vaPutImage pre-encoder upload accepted surface=%u candidate=%d force=%d\n",
                surface_id, surface.encoder_upload_candidate, force_putimage);
        }
    }
    if (surface.status == VASurfaceRendering) {
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    // The initial encoder upload path supports full-frame NV12 copies only.
    // Cropping and scaling can be added later without changing the V4L2 path.
    if (src_x != 0 || src_y != 0 || dst_x != 0 || dst_y != 0 || src_width != image.width
        || src_height != image.height || dst_width != surface.width || dst_height != surface.height
        || src_width != dst_width || src_height != dst_height || image.format.fourcc != VA_FOURCC_NV12
        || image.num_planes != 2 || image.width != surface.width || image.height != surface.height) {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    if (!driver_data->buffers.contains(image.buf)) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    auto& source_buffer = driver_data->buffers.at(image.buf);
    if (!source_buffer.data) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    try {
        // Allocate two DRM planes at upload time, before the first vaBeginPicture.
        // This preserves the existing vaPutImage() path used by FFmpeg hwupload.
        try {
            maybe_allocate_encoder_prime_surface(surface);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "libva-v4l2: direct PRIME surface unavailable: %s; using CPU staging\n", e.what());
        }
        if (!surface.destination_buffer && !surface.converted_buffer && surface.encoder_drm_planes.empty()) {
            if (surface.imported_buffer) {
                surface.converted_buffer = surface.imported_buffer;
                surface.logical_destination_layout = surface.imported_layout;
            } else if (surface.export_buffer) {
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
            } else {
                surface.converted_buffer = std::make_shared<DmaBuffer>(
                    DmaBuffer::allocate(static_cast<size_t>(surface.width) * surface.height * 3 / 2));
                surface.logical_destination_layout = nv12_layout_with_pitch(surface.width, surface.height);
            }
        }

        if (prime_trace_event()) {
            std::fprintf(stderr, "libva-v4l2: vaPutImage target selected: %s\n",
                surface.encoder_drm_planes.size() == 2 ? "two-plane direct PRIME" :
                surface.converted_buffer ? "single-buffer NV12 staging" : "other");
        }
        const auto destination_maps = surface.picture_mapping();
        const auto& layout = surface.logical_destination_layout;
        if (layout.size() != 2 || destination_maps.empty()) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        // A derived image may already refer to this exact surface. There is
        // nothing to copy in that case.
        if (source_buffer.sync_buffer && surface.converted_buffer
            && source_buffer.sync_buffer == surface.converted_buffer) {
            return VA_STATUS_SUCCESS;
        }

        bool source_sync_started = false;
        bool target_sync_started = false;
        std::unique_ptr<EncoderPlaneCpuAccess> direct_sync;
        const bool target_sync_needed = static_cast<bool>(surface.converted_buffer);
        try {
            if (source_buffer.sync_buffer && !source_buffer.cpu_write_mapped) {
                source_buffer.sync_buffer->begin_cpu_access(false);
                source_sync_started = true;
            }
            if (target_sync_needed) {
                surface.converted_buffer->begin_cpu_access(true);
                target_sync_started = true;
            }
            if (!surface.encoder_drm_planes.empty()) {
                direct_sync = std::make_unique<EncoderPlaneCpuAccess>(surface.encoder_drm_planes, true);
            }

            for (unsigned plane = 0; plane < 2; plane++) {
                const unsigned rows = plane == 0 ? surface.height : surface.height / 2;
                const unsigned row_bytes = surface.width;
                const auto& destination = layout[plane];
                if (destination.physical_plane_index >= destination_maps.size() || destination.pitch < row_bytes
                    || destination.size < row_bytes
                    || static_cast<size_t>(rows - 1) > (destination.size - row_bytes) / destination.pitch
                    || !image_destination_fits(source_buffer.size, image.offsets[plane], image.pitches[plane], rows,
                        row_bytes)) {
                    throw std::runtime_error("NV12 image or surface plane has an invalid pitch/size");
                }

                const auto source_available = source_buffer.size - image.offsets[plane];
                if (static_cast<size_t>(rows - 1) * image.pitches[plane] + row_bytes > source_available) {
                    throw std::runtime_error("NV12 image plane extends past its buffer");
                }
                const auto target = destination_maps[destination.physical_plane_index];
                if (destination.offset > target.size()
                    || static_cast<size_t>(rows - 1) * destination.pitch + row_bytes > target.size() - destination.offset) {
                    throw std::runtime_error("NV12 surface plane extends past its backing buffer");
                }

                const uint8_t* source = source_buffer.data.get() + image.offsets[plane];
                uint8_t* dest = target.data() + destination.offset;
                for (unsigned row = 0; row < rows; row++) {
                    std::memmove(dest + static_cast<size_t>(row) * destination.pitch,
                        source + static_cast<size_t>(row) * image.pitches[plane], row_bytes);
                }
            }
            if (direct_sync)
                direct_sync->finish();
        } catch (...) {
            if (target_sync_started) {
                try { surface.converted_buffer->end_cpu_access(true); } catch (...) { }
            }
            if (source_sync_started) {
                try { source_buffer.sync_buffer->end_cpu_access(false); } catch (...) { }
            }
            throw;
        }
        std::exception_ptr sync_error;
        if (target_sync_started) {
            try {
                surface.converted_buffer->end_cpu_access(true);
            } catch (...) {
                sync_error = std::current_exception();
            }
        }
        if (source_sync_started) {
            try {
                source_buffer.sync_buffer->end_cpu_access(false);
            } catch (...) {
                if (!sync_error) {
                    sync_error = std::current_exception();
                }
            }
        }
        if (sync_error) {
            std::rethrow_exception(sync_error);
        }
    } catch (const std::exception& e) {
        error_log(context, "Unable to upload NV12 image to surface: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return VA_STATUS_SUCCESS;
}
