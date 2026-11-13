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

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

extern "C" {
#include <va/va_backend.h>
}

#include "buffer.h"
#include "format.h"
#include "mdp3.h"
#include "v4l2.h"

struct DriverData;

// The picture sizes the hardware decodes for a profile, if the driver says (it does for the MediaTek decoders: up to
// 2048x1088, which rules out portrait full HD video, for example).
std::optional<V4L2M2MDevice::FrameSizeLimits> frame_size_limits(DriverData* driver_data, VAProfile profile);
std::optional<V4L2M2MDevice::FrameSizeLimits> frame_size_limits(const V4L2M2MDevice& device, VAProfile profile);

class ResolutionNotSupported : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Context {
public:
    static Context* create(DriverData* driver_data, VAProfile profile, VAEntrypoint entrypoint, int picture_width,
        int picture_height, std::span<VASurfaceID> surface_ids);
    static std::set<VAProfile> supported_profiles(const std::vector<V4L2M2MDevice>& devices);

    Context(DriverData* driver_data, V4L2M2MDevice& device, fourcc pixelformat, int picture_width, int picture_height,
        std::span<VASurfaceID> surface_ids);
    virtual ~Context();

    // Makes sure the surface has V4L2 buffers to decode into. Applications that create the context without
    // listing the surfaces (e.g. ffmpeg with a dynamic pool) only get buffers once a surface is first used.
    // Throws if no free buffer is left.
    void bind_surface(VASurfaceID surface_id);

    // The hardware converter (MDP3) which can produce exportable pictures, created when first needed. Returns null,
    // and says why in `error`, if there is none.
    Mdp3Converter* mdp3_converter(std::string& error);

    // Called when the converter failed: don't ask it again (it won't get any better), the reason is kept.
    void disable_mdp3(const std::string& reason);

    // Number of the V4L2 buffers a surface decodes into, if it has any.
    std::optional<unsigned> buffer_index(VASurfaceID surface_id) const;

    virtual VAStatus store_buffer(const Buffer& buffer) const = 0;
    virtual int set_controls() = 0;

    VASurfaceID render_surface_id;
    int picture_width;
    int picture_height;
    DriverData* driver_data;
    V4L2M2MDevice& device;

    // The context's own instance of the device, opened for it alone (the ones in DriverData are only there to find out
    // what the hardware can do). Surfaces keep it alive for as long as they refer to its buffers.
    std::shared_ptr<V4L2M2MDevice> device_owner;

    // Pictures in a row for which the decoder reported an error.
    unsigned decode_errors = 0;

    // The common base class also owns an M2M device for encoders.  Their queue
    // lifetime is managed by the encoder subclass, not by the decoder pool.
    bool encoding = false;

private:
    // Number of buffers requested when they have to be allocated without knowing the surfaces (more are added when
    // they run out, as far as the driver lets us). LIBVA_V4L2_BUFFERS overrides it.
    static unsigned deferred_buffer_count();
    // Asks the driver for more buffers; returns whether there are more now.
    bool grow_buffers(std::string& error);

    std::optional<CaptureSetup> capture_setup;
    std::map<VASurfaceID, unsigned> bound_surfaces; // surface -> index of its V4L2 buffers
    mutable std::mutex bound_surfaces_mutex; // (the application may ask for pictures from another thread)

    std::unique_ptr<Mdp3Converter> mdp3;
    std::string mdp3_error;
    bool mdp3_tried = false;
    std::mutex mdp3_mutex;
};

VAStatus createContext(VADriverContextP va_context, VAConfigID config_id, int picture_width, int picture_height,
    int flags, VASurfaceID* surfaces_ids, int surfaces_count, VAContextID* context_id);
VAStatus destroyContext(VADriverContextP va_context, VAContextID context_id);
