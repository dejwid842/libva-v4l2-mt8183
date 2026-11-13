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

#include "picture.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <system_error>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <va/va.h>
}

#include "context.h"
#include "h264_encode.h"
#include "debug.h"
#include "driver.h"
#include "media.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"

using fourcc = uint32_t;

VAStatus beginPicture(VADriverContextP va_context, VAContextID context_id, VASurfaceID surface_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    auto& context = *driver_data->contexts.at(context_id);

    if (!driver_data->surfaces.contains(surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(surface_id);

    if (surface.queued) {
        // Left over from a failure.
        releaseSurfaceBuffers(surface);
    }
    if (surface.pending_sync) {
        // Decoded earlier but never synchronised (e.g. a picture the application didn't need), and now reused.
        try {
            waitForConversion(surface); // (its buffer is about to be overwritten)
        } catch (std::exception&) { }
        surface.pending_sync = false;
        surface.status = VASurfaceReady;
    } else if (surface.status == VASurfaceRendering) {
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    debug::begin_picture();

    if (auto* encoder = dynamic_cast<H264EncoderContext*>(&context)) {
        const auto prepared = encoder->prepare_surface(surface_id);
        if (prepared != VA_STATUS_SUCCESS) {
            return prepared;
        }
        encoder->begin_picture();
        surface.status = VASurfaceRendering;
        context.render_surface_id = surface_id;
        return VA_STATUS_SUCCESS;
    }

    try {
        context.bind_surface(surface_id);
    } catch (std::exception& e) {
        error_log(va_context, "Unable to prepare surface for decoding: %s\n", e.what());
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    surface.status = VASurfaceRendering;
    context.render_surface_id = surface_id;

    return VA_STATUS_SUCCESS;
}

VAStatus renderPicture(VADriverContextP va_context, VAContextID context_id, VABufferID* buffers_ids, int buffers_count)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);
    int rc;
    int i;

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    const auto& context = *driver_data->contexts.at(context_id);

    if (!driver_data->surfaces.contains(context.render_surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    for (i = 0; i < buffers_count; i++) {
        if (!driver_data->buffers.contains(buffers_ids[i])) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        rc = context.store_buffer(driver_data->buffers.at(buffers_ids[i]));
        if (rc != VA_STATUS_SUCCESS)
            return rc;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus endPicture(VADriverContextP va_context, VAContextID context_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);
    VAStatus status;

    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    auto& context = *driver_data->contexts.at(context_id);
    if (!driver_data->surfaces.contains(context.render_surface_id)) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    auto& surface = driver_data->surfaces.at(context.render_surface_id);

    // Whatever goes wrong, don't leave the surface (and context) stuck in the middle of a picture.
    const auto abort_picture = [&](VAStatus result) {
        surface.source_size_used = 0;
        surface.status = VASurfaceReady;
        context.render_surface_id = VA_INVALID_ID;
        memset(&surface.params, 0, sizeof(surface.params));
        return result;
    };

    gettimeofday(&surface.timestamp, NULL);

    if (auto* encoder = dynamic_cast<H264EncoderContext*>(&context)) {
        status = encoder->encode_picture(va_context);
        if (status != VA_STATUS_SUCCESS) {
            return abort_picture(status);
        }
        surface.source_size_used = 0;
        surface.status = VASurfaceReady;
        context.render_surface_id = VA_INVALID_ID;
        memset(&surface.params, 0, sizeof(surface.params));
        return VA_STATUS_SUCCESS;
    }

    if (context.device.media_fd >= 0) {
        try {
            if (surface.request_fd < 0) {
                surface.request_fd = media_request_alloc(context.device.media_fd);
            }
        } catch (std::system_error& e) {
            error_log(va_context, "Unable to allocate media request: %s\n", e.what());
            return abort_picture(VA_STATUS_ERROR_OPERATION_FAILED);
        }

        status = context.set_controls();
        if (status != VA_STATUS_SUCCESS) {
            error_log(va_context, "Unable to set decoding controls\n");
            return abort_picture(status);
        }
    }

    try {
        surface.destination_buffer->get().queue();
    } catch (std::system_error& e) {
        std::string planes;
        for (auto&& plane : surface.destination_buffer->get().mapping()) {
            planes += " " + std::to_string(plane.size());
        }
        error_log(va_context, "Unable to queue capture buffer: %s (buffer planes:%s; driver capture format now: %s)\n",
            e.what(), planes.c_str(), context.device.describe_format(context.device.capture_buf_type).c_str());
        return abort_picture(VA_STATUS_ERROR_OPERATION_FAILED);
    }
    // Exactly what the decoder is about to receive (see LIBVA_V4L2_DUMP in the README).
    debug::dump_file("source.bin", surface.source_buffer->get().mapping()[0].data(), surface.source_size_used);

    surface.queued = true; // from here on the buffers have to be dequeued, or released, before they can be reused
    try {
        surface.source_buffer->get().queue(surface.request_fd, &surface.timestamp, surface.source_size_used);
    } catch (std::system_error& e) {
        error_log(va_context, "Unable to queue output buffer (request fd %d, %u bytes): %s\n", surface.request_fd,
            surface.source_size_used, e.what());
        return abort_picture(VA_STATUS_ERROR_OPERATION_FAILED);
    }

    if (surface.request_fd >= 0) {
        const auto started = std::chrono::steady_clock::now();
        try {
            media_request_queue(surface.request_fd);
            media_request_wait_completion(surface.request_fd);
            media_request_reinit(surface.request_fd);
        } catch (std::runtime_error& e) {
            close(surface.request_fd);
            surface.request_fd = -1;
            error_log(va_context, "Failed to process request: %s\n", e.what());
            debug::dump_failure(surface.source_buffer->get().mapping()[0].data(), surface.source_size_used, e.what());
            return abort_picture(VA_STATUS_ERROR_OPERATION_FAILED);
        }
        debug::add_timing("hardware decoding of a picture",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());

        // The request is complete, so the decoder is done with the buffers: take them back right away. They then
        // come back in decoding order (rather than in the order the application happens to ask for the pictures),
        // and if the hardware reports an error, everything about that picture is still at hand.
        bool decode_error = false;
        std::string decode_error_text;
        for (const auto& buffer : { surface.source_buffer, surface.destination_buffer }) {
            try {
                buffer->get().dequeue();
            } catch (std::runtime_error& e) {
                decode_error = true;
                decode_error_text = e.what();
            }
        }
        surface.queued = false;
        surface.pending_sync = true;

        if (decode_error) {
            // The picture is probably damaged, but showing it is better than giving up on hardware decoding
            // altogether, so only do the latter if it keeps happening.
            error_log(va_context, "The decoder reported an error for a picture (%u in a row): %s\n",
                ++context.decode_errors, decode_error_text.c_str());
            debug::dump_failure(
                surface.source_buffer->get().mapping()[0].data(), surface.source_size_used, decode_error_text.c_str());
            if (context.decode_errors >= 8) {
                return abort_picture(VA_STATUS_ERROR_DECODING_ERROR);
            }
        } else {
            context.decode_errors = 0;
        }

        // Formats which need it are converted in the background, while the application goes on with the next
        // picture; vaSyncSurface (or the export) waits for the result.
        try {
            startConversion(surface);
        } catch (std::exception& e) {
            error_log(va_context, "Unable to convert the decoded picture: %s\n", e.what());
            return abort_picture(VA_STATUS_ERROR_OPERATION_FAILED);
        }
    }

    surface.source_size_used = 0;

    context.render_surface_id = VA_INVALID_ID;
    memset(&surface.params, 0, sizeof(surface.params));

    return VA_STATUS_SUCCESS;
}
