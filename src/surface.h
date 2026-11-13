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

#pragma once

#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <vector>

extern "C" {
#include <linux/videodev2.h>

#include <va/va_backend.h>
}

#include "context.h"
#include "dmabuf.h"
#include "drm.h"
#include "format.h"
#include "v4l2.h"

struct DriverData;

struct Surface {
    VASurfaceStatus status;
    unsigned width;
    unsigned height;

    // Set by an encode context: these surfaces are raw NV12 inputs, not decoder targets.
    bool encoder_input = false;
    // Surface-purpose classification captured at vaCreateSurfaces2 time.
    // Used by the scoped Direct PRIME upload selection before vaCreateContext.
    bool encoder_upload_candidate = false;
    unsigned int usage_hint = VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC;

    // Optional encoder-owned linear NV12M backing. Two distinct DRM PRIME
    // objects can be imported by V4L2 without an additional staging copy.
    // This does not remove FFmpeg's system-memory -> VA surface upload.
    std::vector<std::shared_ptr<DrmBuffer>> encoder_drm_planes;

    std::optional<std::reference_wrapper<const V4L2M2MDevice::Buffer>> source_buffer;
    unsigned int source_size_used;

    std::optional<std::reference_wrapper<const V4L2M2MDevice::Buffer>> destination_buffer;
    BufferLayout logical_destination_layout;
    uint32_t format;

    timeval timestamp;

    union {
        struct {
            VAPictureParameterBufferMPEG2* picture;
            VASliceParameterBufferMPEG2* slice;
            VAIQMatrixBufferMPEG2* iqmatrix;
        } mpeg2;
        struct {
            VAIQMatrixBufferH264* matrix;
            VAPictureParameterBufferH264* picture;
            VASliceParameterBufferH264* slice;
        } h264;
        struct {
            VAPictureParameterBufferVP8* picture;
            VASliceParameterBufferVP8* slice;
            VAProbabilityDataBufferVP8* probabilities;
            VAIQMatrixBufferVP8* iqmatrix;
        } vp8;
        struct {
            VADecPictureParameterBufferVP9* picture;
            VASliceParameterBufferVP9* slice;
        } vp9;
    } params;

    int request_fd;

    // True while the buffers of the last decoded picture still sit in the V4L2 queues, i.e. from vaEndPicture
    // until they are dequeued by vaSyncSurface (or released, see releaseSurfaceBuffers).
    bool queued = false;

    // The picture has been decoded, but vaSyncSurface wasn't called for it yet (which is where converted formats
    // are converted).
    bool pending_sync = false;

    // Capture formats that can't be used as is (e.g. MM21, tiled) are converted by the CPU into this buffer after
    // decoding. `logical_destination_layout` then describes this buffer, rather than the V4L2 one.
    Conversion conversion = Conversion::None;
    // Opt-in: original MM21 is retained until the encoder detiles directly into its input buffers.
    bool deferred_encoder_conversion = false;
    std::shared_ptr<DmaBuffer> converted_buffer;

    // The device the surface's V4L2 buffers belong to (it goes away with its context, but they must not).
    std::shared_ptr<V4L2M2MDevice> device;

    // Where the converted picture is copied for exporting, if our own buffer for it is no DMA-BUF. Once something has
    // been exported, that is the buffer which belongs to the surface: every picture decoded into the surface is copied
    // there as well (applications export a surface once and expect the pictures to show up in it), in the layout the
    // export described (`export_layout`).
    std::shared_ptr<DrmBuffer> export_buffer;
    BufferLayout export_layout;

    // A buffer the application made for the surface (import mode), which the pictures have to be put into, in the
    // layout the application described.
    std::shared_ptr<DmaBuffer> imported_buffer;
    BufferLayout imported_layout;

    // The application can look at the surface without synchronising it (it was exported before decoding, or the
    // application's own buffer is used), so the picture must be complete when decoding is.
    bool complete_before_sync = false;

    // The conversion of the last decoded picture, running in the background (see startConversion()).
    std::shared_future<void> conversion_result;

    // Memory holding the finished picture, as described by `logical_destination_layout`.
    // Empty if the surface never had anything decoded into it.
    std::vector<std::span<uint8_t>> picture_mapping() const
    {
        if (encoder_drm_planes.size() == 2) {
            return { { encoder_drm_planes[0]->data(), encoder_drm_planes[0]->size() },
                { encoder_drm_planes[1]->data(), encoder_drm_planes[1]->size() } };
        }
        if (converted_buffer) {
            return { { converted_buffer->data(), converted_buffer->size() } };
        }
        if (destination_buffer) {
            return destination_buffer->get().mapping();
        }
        if (export_buffer) {
            return { { export_buffer->data(), export_buffer->size() } };
        }
        return {};
    }
};

// Converts the decoded picture (if its format needs it) in a separate thread, so that it overlaps with whatever the
// application does next, such as the decoding of the next picture. Throws if the conversion can't be set up.
void startConversion(Surface& surface);
// Waits for that conversion to finish; throws if it failed. Does nothing if there is none.
void waitForConversion(Surface& surface);
// Takes the buffers of a decoded, but never synchronised picture back from the driver, so they can be queued again.
void releaseSurfaceBuffers(Surface& surface);
// Sets the capture format of the device and allocates `count` capture buffers.
CaptureSetup setupCapture(const Context& context, const Surface& reference, unsigned count);
// Attaches capture buffer number `index` (and, if needed, a conversion buffer) to the surface.
void bindSurface(Surface& surface, const Context& context, const CaptureSetup& setup, unsigned index);
// Does both of the above for surfaces known when the context is created.
CaptureSetup createSurfacesDeferred(DriverData* driver_data, const Context& context, std::span<VASurfaceID> surface_ids);
VAStatus createSurfaces2(VADriverContextP context, unsigned int format, unsigned int width, unsigned int height,
    VASurfaceID* surfaces_ids, unsigned int surfaces_count, VASurfaceAttrib* attributes, unsigned int attributes_count);
VAStatus createSurfaces(
    VADriverContextP context, int width, int height, int format, int surfaces_count, VASurfaceID* surfaces_ids);
VAStatus destroySurfaces(VADriverContextP context, VASurfaceID* surfaces_ids, int surfaces_count);
VAStatus syncSurface(VADriverContextP context, VASurfaceID surface_id);
VAStatus querySurfaceAttributes(
    VADriverContextP context, VAConfigID config, VASurfaceAttrib* attributes, unsigned int* attributes_count);
VAStatus querySurfaceStatus(VADriverContextP context, VASurfaceID surface_id, VASurfaceStatus* status);
VAStatus putSurface(VADriverContextP context, VASurfaceID surface_id, void* draw, short src_x, short src_y,
    unsigned short src_width, unsigned short src_height, short dst_x, short dst_y, unsigned short dst_width,
    unsigned short dst_height, VARectangle* cliprects, unsigned int cliprects_count, unsigned int flags);
VAStatus lockSurface(VADriverContextP context, VASurfaceID surface_id, unsigned int* fourcc, unsigned int* luma_stride,
    unsigned int* chroma_u_stride, unsigned int* chroma_v_stride, unsigned int* luma_offset,
    unsigned int* chroma_u_offset, unsigned int* chroma_v_offset, unsigned int* buffer_name, void** buffer);
VAStatus unlockSurface(VADriverContextP context, VASurfaceID surface_id);
VAStatus exportSurfaceHandle(
    VADriverContextP context, VASurfaceID surface_id, uint32_t mem_type, uint32_t flags, void* descriptor);

// Complete an opt-in deferred conversion before using the linear VA surface.
void materializeDeferredConversion(Surface& surface);
// Detile a decoder MM21 capture frame into two independently mapped NV12M planes.
void detileDecoderIntoNV12M(const Surface& surface, std::span<uint8_t> y, unsigned y_pitch,
    std::span<uint8_t> uv, unsigned uv_pitch, unsigned width, unsigned height, unsigned group_tiles = 1, bool row_major = false, unsigned prefetch_tiles = 0);
