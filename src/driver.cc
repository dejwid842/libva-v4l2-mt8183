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

#include "driver.h"
#include "debug.h"

#include <cassert>
#include <exception>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_backend.h>
}

#include "buffer.h"
#include "config.h"
#include "context.h"
#include "image.h"
#include "picture.h"
#include "subpicture.h"
#include "surface.h"
#include "utils.h"

namespace {

std::optional<std::string> getenv_opt(const std::string& name)
{
    auto val = getenv(name.c_str());
    return val ? std::optional<std::string>(val) : std::optional<std::string>();
}

} // namespace

DriverData::DriverData(const std::vector<std::pair<std::string, std::optional<std::string>>>& device_paths)
    : devices()
{
    for (auto&& [video_path, media_path] : device_paths) {
        devices.emplace_back(video_path, media_path);
    }
}

/* Set default visibility for the init function only. */
namespace {

// Exceptions must never escape through the C API of libva (that would terminate the whole application).
template <auto F, typename = decltype(F)> struct Guarded;
template <auto F, typename... Args> struct Guarded<F, VAStatus (*)(VADriverContextP, Args...)> {
    static VAStatus call(VADriverContextP context, Args... args)
    {
        try {
            return F(context, args...);
        } catch (const std::exception& e) {
            error_log(context, "Unhandled exception: %s\n", e.what());
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
};

} // namespace

VAStatus __attribute__((visibility("default"))) VA_DRIVER_INIT_FUNC(VADriverContextP context);

extern "C" VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP context)
{
    debug::install_crash_handler();

    auto devices = V4L2M2MDevice::enumerate_devices();

    if (const auto video_path_env = getenv_opt("LIBVA_V4L2_VIDEO_PATH"); video_path_env) {
        const auto media_path_env = getenv_opt("LIBVA_V4L2_MEDIA_PATH");
        info_log(context, "Overriding V4L2 device with %s & %s.\n", video_path_env.value().c_str(),
            media_path_env.value_or("").c_str());
        devices.clear();
        devices.push_back({ video_path_env.value(), media_path_env });
    }
    auto driver_data = new DriverData(devices);

    struct VADriverVTable* vtable = context->vtable;

    context->version_major = VA_MAJOR_VERSION;
    context->version_minor = VA_MINOR_VERSION;
    context->max_profiles = V4L2_MAX_PROFILES;
    context->max_entrypoints = V4L2_MAX_ENTRYPOINTS;
    context->max_attributes = Config::max_attributes;
    context->max_image_formats = V4L2_MAX_IMAGE_FORMATS;
    context->max_subpic_formats = V4L2_MAX_SUBPIC_FORMATS;
    context->max_display_attributes = V4L2_MAX_DISPLAY_ATTRIBUTES;
    context->str_vendor = V4L2_STR_VENDOR;

    vtable->vaTerminate = Guarded<terminate>::call;
    vtable->vaQueryConfigEntrypoints = Guarded<queryConfigEntrypoints>::call;
    vtable->vaQueryConfigProfiles = Guarded<queryConfigProfiles>::call;
    vtable->vaQueryConfigEntrypoints = Guarded<queryConfigEntrypoints>::call;
    vtable->vaQueryConfigAttributes = Guarded<queryConfigAttributes>::call;
    vtable->vaCreateConfig = Guarded<createConfig>::call;
    vtable->vaDestroyConfig = Guarded<destroyConfig>::call;
    vtable->vaGetConfigAttributes = Guarded<getConfigAttributes>::call;
    vtable->vaCreateSurfaces = Guarded<createSurfaces>::call;
    vtable->vaCreateSurfaces2 = Guarded<createSurfaces2>::call;
    vtable->vaDestroySurfaces = Guarded<destroySurfaces>::call;
    vtable->vaExportSurfaceHandle = Guarded<exportSurfaceHandle>::call;
    vtable->vaCreateContext = Guarded<createContext>::call;
    vtable->vaDestroyContext = Guarded<destroyContext>::call;
    vtable->vaCreateBuffer = Guarded<createBuffer>::call;
    vtable->vaBufferSetNumElements = Guarded<bufferSetNumElements>::call;
    vtable->vaMapBuffer = Guarded<mapBuffer>::call;
    vtable->vaUnmapBuffer = Guarded<unmapBuffer>::call;
    vtable->vaDestroyBuffer = Guarded<destroyBuffer>::call;
    vtable->vaBufferInfo = Guarded<bufferInfo>::call;
    vtable->vaAcquireBufferHandle = Guarded<acquireBufferHandle>::call;
    vtable->vaReleaseBufferHandle = Guarded<releaseBufferHandle>::call;
    vtable->vaBeginPicture = Guarded<beginPicture>::call;
    vtable->vaRenderPicture = Guarded<renderPicture>::call;
    vtable->vaEndPicture = Guarded<endPicture>::call;
    vtable->vaSyncSurface = Guarded<syncSurface>::call;
    vtable->vaQuerySurfaceAttributes = Guarded<querySurfaceAttributes>::call;
    vtable->vaQuerySurfaceStatus = Guarded<querySurfaceStatus>::call;
    vtable->vaPutSurface = Guarded<putSurface>::call;
    vtable->vaQueryImageFormats = Guarded<queryImageFormats>::call;
    vtable->vaCreateImage = Guarded<createImage>::call;
    vtable->vaDeriveImage = Guarded<deriveImage>::call;
    vtable->vaDestroyImage = Guarded<destroyImage>::call;
    vtable->vaSetImagePalette = Guarded<setImagePalette>::call;
    vtable->vaGetImage = Guarded<getImage>::call;
    vtable->vaPutImage = Guarded<putImage>::call;
    vtable->vaQuerySubpictureFormats = Guarded<querySubpictureFormats>::call;
    vtable->vaCreateSubpicture = Guarded<createSubpicture>::call;
    vtable->vaDestroySubpicture = Guarded<destroySubpicture>::call;
    vtable->vaSetSubpictureImage = Guarded<setSubpictureImage>::call;
    vtable->vaSetSubpictureChromakey = Guarded<setSubpictureChromakey>::call;
    vtable->vaSetSubpictureGlobalAlpha = Guarded<setSubpictureGlobalAlpha>::call;
    vtable->vaAssociateSubpicture = Guarded<associateSubpicture>::call;
    vtable->vaDeassociateSubpicture = Guarded<deassociateSubpicture>::call;
    vtable->vaQueryDisplayAttributes = Guarded<queryDisplayAttributes>::call;
    vtable->vaGetDisplayAttributes = Guarded<getDisplayAttributes>::call;
    vtable->vaSetDisplayAttributes = Guarded<setDisplayAttributes>::call;
    vtable->vaLockSurface = Guarded<lockSurface>::call;
    vtable->vaUnlockSurface = Guarded<unlockSurface>::call;

    context->pDriverData = driver_data;

    return VA_STATUS_SUCCESS;
}

VAStatus terminate(VADriverContextP va_context)
{
    auto driver_data = static_cast<DriverData*>(va_context->pDriverData);

    /* Cleanup leftover buffers. */
    for (auto&& [id, config] : driver_data->configs) {
        destroyConfig(va_context, id);
    }

    for (auto&& [id, ctx] : driver_data->contexts) {
        destroyContext(va_context, id);
    }

    for (auto&& [id, surface] : driver_data->surfaces) {
        VASurfaceID id_ = id;
        destroySurfaces(va_context, &id_, 1);
    }

    for (auto&& [id, buffer] : driver_data->buffers) {
        destroyBuffer(va_context, id);
    }

    for (auto&& [id, image] : driver_data->images) {
        destroyImage(va_context, id);
    }

    delete driver_data;
    va_context->pDriverData = nullptr;

    return VA_STATUS_SUCCESS;
}
