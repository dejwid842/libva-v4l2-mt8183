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

#include "config.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <span>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include <va/va.h>
}

#include "context.h"
#include "driver.h"
#include "h264_encode.h"
#include "utils.h"

namespace {

std::set<VAProfile> supported_profiles(const DriverData* driver_data)
{
    auto result = Context::supported_profiles(driver_data->devices);
    for (const auto& device : driver_data->devices) {
        const auto encoder_profiles = H264EncoderContext::supported_profiles(device);
        result.insert(encoder_profiles.begin(), encoder_profiles.end());
    }
    return result;
}

bool supports_entrypoint(const DriverData* driver_data, VAProfile profile, VAEntrypoint entrypoint)
{
    if (entrypoint == VAEntrypointVLD) {
        return Context::supported_profiles(driver_data->devices).contains(profile);
    }
    if (entrypoint == VAEntrypointEncSlice) {
        return std::ranges::any_of(driver_data->devices, [&](const auto& device) {
            return H264EncoderContext::supported_profiles(device).contains(profile);
        });
    }
    return false;
}

} // namespace

VAStatus createConfig(VADriverContextP context, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attributes,
    int attributes_count, VAConfigID* config_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    int i, index;

    if (entrypoint != VAEntrypointVLD && entrypoint != VAEntrypointEncSlice) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if (!supports_entrypoint(driver_data, profile, entrypoint)) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (static_cast<unsigned>(attributes_count) > Config::max_attributes) {
        attributes_count = Config::max_attributes;
    }

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    *config_id = smallest_free_key(driver_data->configs);
    auto [config, inserted] = driver_data->configs.emplace(std::make_pair(*config_id,
        Config {
            .profile = profile,
            .entrypoint = entrypoint,
            .attributes { { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 } },
            .attributes_count = 1,
        }));
    if (!inserted) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    for (i = 1; i < attributes_count; i++) {
        index = config->second.attributes_count++;
        config->second.attributes[index].type = attributes[index].type;
        config->second.attributes[index].value = attributes[index].value;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus destroyConfig(VADriverContextP context, VAConfigID config_id)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    std::lock_guard<std::mutex> guard(driver_data->mutex);
    if (!driver_data->configs.erase(config_id)) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigProfiles(VADriverContextP context, VAProfile* profiles_, int* profile_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    std::span<VAProfile> profiles(profiles_, V4L2_MAX_PROFILES);
    const auto supported = ::supported_profiles(driver_data);

    *profile_count = std::min(profiles.size(), supported.size());

    std::ranges::copy_n(supported.begin(), *profile_count, profiles.begin());

    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigEntrypoints(
    VADriverContextP context, VAProfile profile, VAEntrypoint* entrypoints, int* entrypoints_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);
    *entrypoints_count = 0;
    if (supports_entrypoint(driver_data, profile, VAEntrypointVLD)) {
        entrypoints[(*entrypoints_count)++] = VAEntrypointVLD;
    }
    if (supports_entrypoint(driver_data, profile, VAEntrypointEncSlice)) {
        entrypoints[(*entrypoints_count)++] = VAEntrypointEncSlice;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigAttributes(VADriverContextP context, VAConfigID config_id, VAProfile* profile,
    VAEntrypoint* entrypoint, VAConfigAttrib* attributes, int* attributes_count)
{
    auto driver_data = static_cast<DriverData*>(context->pDriverData);

    if (!driver_data->configs.contains(config_id)) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    const auto& config = driver_data->configs.at(config_id);

    if (profile != NULL)
        *profile = config.profile;

    if (entrypoint != NULL)
        *entrypoint = config.entrypoint;

    if (attributes_count != NULL) {
        *attributes_count = config.attributes_count;
    }

    /* Attributes might be NULL to retrieve the associated count. */
    if (attributes != NULL) {
        std::copy_n(config.attributes, config.attributes_count, attributes);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus getConfigAttributes(VADriverContextP context, VAProfile profile, VAEntrypoint entrypoint,
    VAConfigAttrib* attributes, int attributes_count)
{
    const auto driver_data = static_cast<DriverData*>(context->pDriverData);
    std::optional<V4L2M2MDevice::FrameSizeLimits> limits;
    if (entrypoint == VAEntrypointVLD) {
        limits = frame_size_limits(const_cast<DriverData*>(driver_data), profile);
    } else if (entrypoint == VAEntrypointEncSlice) {
        for (const auto& device : driver_data->devices) {
            if (H264EncoderContext::supported_profiles(device).contains(profile)) {
                limits = device.frame_size_limits(V4L2_PIX_FMT_NV12M);
                break;
            }
        }
    }

    for (int i = 0; i < attributes_count; i++) {
        switch (attributes[i].type) {
        case VAConfigAttribRTFormat:
            attributes[i].value = VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[i].value = limits ? limits->max_width : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[i].value = limits ? limits->max_height : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribRateControl:
            attributes[i].value = entrypoint == VAEntrypointEncSlice ? VA_RC_CBR : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxRefFrames:
            // Stateful MT8183 VENC encodes I/P pictures.  Advertise one
            // past-frame L0 reference and no L1 (B-picture) references.
            // VA-API packs L0 in bits 0-15 and L1 in bits 16-31.
            attributes[i].value = entrypoint == VAEntrypointEncSlice ? 1u : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncPackedHeaders:
            // The MT8183 firmware generates SPS/PPS itself.  Packed headers
            // supplied by applications are therefore not advertised.
            attributes[i].value = entrypoint == VAEntrypointEncSlice ? 0 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        default:
            attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus queryDisplayAttributes(VADriverContextP context, VADisplayAttribute* attributes, int* attributes_count)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus getDisplayAttributes(VADriverContextP context, VADisplayAttribute* attributes, int attributes_count)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus setDisplayAttributes(VADriverContextP context, VADisplayAttribute* attributes, int attributes_count)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
