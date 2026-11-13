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

#include <string>
#include <algorithm>
#include <memory>
#include <stdexcept>

#include "context.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va.h>
}

#include "config.h"
#include "driver.h"
#include "h264.h"
#include "h264_encode.h"
#include "mpeg2.h"
#include "surface.h"
#include "utils.h"
#include "v4l2.h"
#include "vp8.h"
#ifdef ENABLE_VP9
#include "vp9.h"
#endif

std::set<VAProfile> Context::supported_profiles(const std::vector<V4L2M2MDevice>& devices)
{
    std::set<VAProfile> result;
    for (auto&& device : devices) {
        for (auto&& profile : MPEG2Context::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : H264Context::supported_profiles(device)) {
            result.insert(profile);
        }
        for (auto&& profile : VP8Context::supported_profiles(device)) {
            result.insert(profile);
        }
#ifdef ENABLE_VP9
        for (auto&& profile : VP9Context::supported_profiles(device)) {
            result.insert(profile);
        }
#endif
    }
    return result;
}

namespace {

bool device_supports_profile(const V4L2M2MDevice& device, VAProfile profile)
{
    if (MPEG2Context::supported_profiles(device).contains(profile)
        || H264Context::supported_profiles(device).contains(profile)
        || VP8Context::supported_profiles(device).contains(profile)) {
        return true;
    }
#ifdef ENABLE_VP9
    return VP9Context::supported_profiles(device).contains(profile);
#else
    return false;
#endif
}

unsigned profile_coded_format(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264MultiviewHigh:
    case VAProfileH264StereoHigh:
        return V4L2_PIX_FMT_H264_SLICE;
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        return V4L2_PIX_FMT_MPEG2_SLICE;
    case VAProfileVP8Version0_3:
        return V4L2_PIX_FMT_VP8_FRAME;
#ifdef ENABLE_VP9
    case VAProfileVP9Profile0:
    case VAProfileVP9Profile1:
    case VAProfileVP9Profile2:
    case VAProfileVP9Profile3:
        return V4L2_PIX_FMT_VP9_FRAME;
#endif
    default:
        return 0;
    }
}

} // namespace

std::optional<V4L2M2MDevice::FrameSizeLimits> frame_size_limits(
    const V4L2M2MDevice& device, VAProfile profile)
{
    const unsigned format = profile_coded_format(profile);
    if (format == 0 || !device_supports_profile(device, profile)) {
        return std::nullopt;
    }
    return device.frame_size_limits(format);
}

std::optional<V4L2M2MDevice::FrameSizeLimits> frame_size_limits(DriverData* driver_data, VAProfile profile)
{
    for (const auto& device : driver_data->devices) {
        if (device_supports_profile(device, profile)) {
            return frame_size_limits(device, profile);
        }
    }
    return std::nullopt;
}

Context* Context::create(DriverData* driver_data, VAProfile profile, VAEntrypoint entrypoint, int picture_width,
    int picture_height, std::span<VASurfaceID> surface_ids)
{
    for (auto&& prototype : driver_data->devices) {
        if (entrypoint == VAEntrypointEncSlice) {
            if (!H264EncoderContext::supported_profiles(prototype).contains(profile)) {
                continue;
            }

            // Check the encoder's raw-input dimensions on the same device that
            // will be opened for the context.  The encoder reports sizes for
            // NV12M, while decoders report sizes for the coded bitstream.
            if (const auto limits = prototype.frame_size_limits(V4L2_PIX_FMT_NV12M);
                limits && !limits->supports(picture_width, picture_height)) {
                throw ResolutionNotSupported("the selected encoder does not support "
                    + std::to_string(picture_width) + "x" + std::to_string(picture_height));
            }

            auto own = std::make_shared<V4L2M2MDevice>(prototype.video_path, prototype.media_path);
            auto* context = new H264EncoderContext(driver_data, *own, profile, picture_width, picture_height);
            context->device_owner = std::move(own);
            if (surface_ids.empty()) {
                // Some clients create the context before their dynamic surface pool.
                // Mark currently unbound surfaces now; createSurfaces2() marks later ones.
                for (auto& [surface_id, surface] : driver_data->surfaces) {
                    if (!surface.device) {
                        surface.encoder_input = true;
                    }
                }
            } else {
                for (const auto surface_id : surface_ids) {
                    if (const auto surface = driver_data->surfaces.find(surface_id);
                        surface != driver_data->surfaces.end()) {
                        surface->second.encoder_input = true;
                    }
                }
            }
            return context;
        }

        if (entrypoint != VAEntrypointVLD) {
            continue;
        }
        const bool mpeg2 = MPEG2Context::supported_profiles(prototype).contains(profile);
        const bool h264 = H264Context::supported_profiles(prototype).contains(profile);
        const bool vp8 = VP8Context::supported_profiles(prototype).contains(profile);
#ifdef ENABLE_VP9
        const bool vp9 = VP9Context::supported_profiles(prototype).contains(profile);
#else
        const bool vp9 = false;
#endif
        if (!mpeg2 && !h264 && !vp8 && !vp9) {
            continue;
        }

        // Check limits on the same device that will actually be opened for this context. Do not use the limits of
        // another decoder instance merely because it advertises the same coded pixel format.
        if (const auto limits = frame_size_limits(prototype, profile);
            limits && !limits->supports(picture_width, picture_height)) {
            throw ResolutionNotSupported("the selected decoder supports the enumerated picture sizes only; "
                "requested " + std::to_string(picture_width) + "x" + std::to_string(picture_height)
                + ", limits are " + std::to_string(limits->min_width) + "x" + std::to_string(limits->min_height)
                + " to " + std::to_string(limits->max_width) + "x" + std::to_string(limits->max_height));
        }

        // Several contexts can be in use at the same time (two videos playing), and each one needs a decoder of its
        // own: opening the device again gives one.
        auto own = std::make_shared<V4L2M2MDevice>(prototype.video_path, prototype.media_path);

        Context* context = nullptr;
        if (mpeg2) {
            context = new MPEG2Context(driver_data, *own, picture_width, picture_height, surface_ids);
        } else if (h264) {
            context = new H264Context(driver_data, *own, profile, picture_width, picture_height, surface_ids);
        } else if (vp8) {
            context = new VP8Context(driver_data, *own, picture_width, picture_height, surface_ids);
        }
#ifdef ENABLE_VP9
        else if (vp9) {
            context = new VP9Context(driver_data, *own, picture_width, picture_height, surface_ids);
        }
#endif
        context->device_owner = std::move(own);

        // Surfaces bound while the context was created (their buffers are the device's) have to keep it alive, too.
        for (const auto surface_id : surface_ids) {
            if (const auto surface = driver_data->surfaces.find(surface_id); surface != driver_data->surfaces.end()) {
                surface->second.device = context->device_owner;
            }
        }
        return context;
    }

    throw std::invalid_argument("Unimplemented profile");
}

Context::Context(DriverData* driver_data, V4L2M2MDevice& dev, fourcc pixelformat, int picture_width, int picture_height,
    std::span<VASurfaceID> surface_ids)
    : render_surface_id(VA_INVALID_ID)
    , picture_width(picture_width)
    , picture_height(picture_height)
    , driver_data(driver_data)
    , device(dev)
{
    device.set_format(device.output_buf_type, pixelformat, picture_width, picture_height);

    if (surface_ids.empty()) {
        // Surfaces are not known yet, buffers get allocated by bind_surface() once they are used.
        return;
    }

    // Now that the output format is set, we can set the capture format and allocate the surfaces.
    capture_setup = createSurfacesDeferred(driver_data, *this, surface_ids);

    const unsigned output_count = device.request_buffers(device.output_buf_type, surface_ids.size());
    if (output_count < surface_ids.size()) {
        throw std::runtime_error("Driver granted fewer output buffers than there are surfaces");
    }

    for (unsigned i = 0; i < surface_ids.size(); i++) {
        driver_data->surfaces.at(surface_ids[i]).source_buffer
            = std::cref(device.buffer(device.output_buf_type, i));
        bound_surfaces[surface_ids[i]] = i;
    }

    device.set_streaming(true);
}

unsigned Context::deferred_buffer_count()
{
    if (const char* value = getenv("LIBVA_V4L2_BUFFERS"); value && *value) {
        if (const long count = strtol(value, nullptr, 10); count > 0 && count <= 128) {
            return count;
        }
    }
    return 24;
}

bool Context::grow_buffers(std::string& error)
{
    constexpr unsigned step = 8;
    try {
        const unsigned capture = device.create_buffers(device.capture_buf_type, step, capture_setup->cpu_cached);
        if (capture > 0) {
            device.create_buffers(device.output_buf_type, capture);
        }
    } catch (const std::exception& e) {
        error = e.what();
    }

    // Surfaces need both kinds, so what counts is what both have.
    const unsigned count = std::min(device.buffer_count(device.capture_buf_type), device.buffer_count(device.output_buf_type));
    const bool grown = count > capture_setup->buffer_count;
    capture_setup->buffer_count = std::max(count, capture_setup->buffer_count);
    if (!grown && error.empty()) {
        error = "the driver gave no more buffers";
    }
    return grown;
}

Mdp3Converter* Context::mdp3_converter(std::string& error)
{
    std::lock_guard<std::mutex> guard(mdp3_mutex);
    if (!mdp3_tried) {
        mdp3_tried = true;
        if (capture_setup) {
            mdp3 = Mdp3Converter::create(device.capture_format.fmt.pix_mp, capture_setup->buffer_count, mdp3_error);
        } else {
            mdp3_error = "nothing has been decoded yet";
        }
    }
    error = mdp3_error;
    return mdp3.get();
}

void Context::disable_mdp3(const std::string& reason)
{
    std::lock_guard<std::mutex> guard(mdp3_mutex);
    mdp3.reset();
    mdp3_tried = true;
    mdp3_error = reason;
}

std::optional<unsigned> Context::buffer_index(VASurfaceID surface_id) const
{
    std::lock_guard<std::mutex> guard(bound_surfaces_mutex);
    if (const auto found = bound_surfaces.find(surface_id); found != bound_surfaces.end()) {
        return found->second;
    }
    return std::nullopt;
}

void Context::bind_surface(VASurfaceID surface_id)
{
    auto& surface = driver_data->surfaces.at(surface_id);
    surface.encoder_input = false;
    if (surface.source_buffer && surface.destination_buffer) {
        return;
    }

    std::lock_guard<std::mutex> guard(bound_surfaces_mutex);

    if (!capture_setup) {
        capture_setup = setupCapture(*this, surface, deferred_buffer_count());
        const unsigned output_count = device.request_buffers(device.output_buf_type, deferred_buffer_count());
        capture_setup->buffer_count = std::min(capture_setup->buffer_count, output_count);
        device.set_streaming(true);
    }

    // Forget buffers of destroyed surfaces (ids get reused, so look at whether the surface is actually bound).
    std::erase_if(bound_surfaces, [&](const auto& entry) {
        const auto it = driver_data->surfaces.find(entry.first);
        return it == driver_data->surfaces.end() || !it->second.source_buffer;
    });

    unsigned index = 0;
    const auto taken = [&](unsigned candidate) {
        return std::ranges::any_of(bound_surfaces, [&](const auto& entry) { return entry.second == candidate; });
    };
    while (index < capture_setup->buffer_count && taken(index)) {
        index++;
    }
    if (index >= capture_setup->buffer_count) {
        // Out of buffers, but the driver might let us have more. (Applications may use many more surfaces than the
        // decoder needs at a time, as a pool of pictures to show.)
        std::string growth_error;
        if (grow_buffers(growth_error)) {
            while (index < capture_setup->buffer_count && taken(index)) {
                index++;
            }
        }
        if (index >= capture_setup->buffer_count) {
            throw std::runtime_error("No free decoder buffers left for another surface (" + std::to_string(bound_surfaces.size())
                + " surfaces are using the " + std::to_string(capture_setup->buffer_count) + " there are"
                + (growth_error.empty() ? "" : "; asking for more failed: " + growth_error) + ")");
        }
    }

    surface.source_buffer = std::cref(device.buffer(device.output_buf_type, index));
    bindSurface(surface, *this, *capture_setup, index);
    bound_surfaces[surface_id] = index;
}

Context::~Context()
{
    if (encoding) {
        return;
    }
    // A conversion running in the background reads from this context's buffers, which are about to go away.
    for (const auto& [surface_id, index] : bound_surfaces) {
        if (const auto surface = driver_data->surfaces.find(surface_id); surface != driver_data->surfaces.end()) {
            try {
                waitForConversion(surface->second);
            } catch (std::exception&) { }
        }
    }

    device.set_streaming(false);
    device.request_buffers(device.capture_buf_type, 0);
}

VAStatus createContext(VADriverContextP va_context, VAConfigID config_id, int picture_width, int picture_height,
    int flags, VASurfaceID* surface_ids, int surfaces_count, VAContextID* context_id)
{
    // FIXME: Should create own V4L2M2MDevice to localize settings?
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    if (!driver_data->configs.contains(config_id)) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    const auto& config = driver_data->configs.at(config_id);

    auto surfaces = std::span(surface_ids, surfaces_count);
    for (auto&& surface : surfaces) {
        if (!driver_data->surfaces.contains(surface)) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    try {
        *context_id = smallest_free_key(driver_data->contexts);
        auto [context, inserted] = driver_data->contexts.emplace(std::make_pair(
            *context_id, Context::create(driver_data, config.profile, config.entrypoint, picture_width, picture_height,
                surfaces)));
        if (!inserted) {
            error_log(va_context, "Failed to create context\n");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    } catch (const ResolutionNotSupported& e) {
        error_log(va_context, "Unable to create context: %s\n", e.what());
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    } catch (std::exception& e) {
        error_log(va_context, "Failed to create context: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus destroyContext(VADriverContextP va_context, VAContextID context_id)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    if (!driver_data->contexts.contains(context_id)) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    driver_data->contexts.erase(context_id);

    return VA_STATUS_SUCCESS;
}
