/*
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

#include "format.h"

#include <algorithm>
#include <stdexcept>

extern "C" {
#include <linux/videodev2.h>

#include <libdrm/drm_fourcc.h>
#include <va/va.h>
}

namespace {

BufferLayout nv12_derive_layout(unsigned width, unsigned height)
{
    return nv12_layout_with_pitch(width, height);
}

} // namespace

BufferLayout nv12_layout_with_pitch(unsigned pitch, unsigned height)
{
    const auto luma_size = pitch * height;
    return BufferLayout {
        { 0, luma_size, pitch, 0 },
        { 0, pitch * ((height + 1) / 2), pitch, luma_size },
    };
}

// Order matters: when a device offers several formats for the same render target, the first match wins.
// Formats that can be used as is are therefore listed before those requiring a software conversion.
const std::array<Format, 3> formats = {
    Format { { V4L2_PIX_FMT_NV12, &nv12_derive_layout, Conversion::None }, { VA_FOURCC_NV12, VA_RT_FORMAT_YUV420 },
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR } },
    Format { { V4L2_PIX_FMT_NV12M, nullptr, Conversion::None }, { VA_FOURCC_NV12, VA_RT_FORMAT_YUV420 },
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR } },
    // The only capture format of the MediaTek MT8183 decoder (mtk-vcodec). The data is converted to linear NV12,
    // so it is exposed to applications exactly like the other NV12 variants.
    Format { { V4L2_PIX_FMT_MM21, nullptr, Conversion::MM21ToNV12 }, { VA_FOURCC_NV12, VA_RT_FORMAT_YUV420 },
        { DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR } },
};

const Format& lookup_format(fourcc v4l2_fourcc)
{
    auto it = std::ranges::find_if(formats, [&](auto&& f) { return f.v4l2.format == v4l2_fourcc; });
    if (it == formats.end()) {
        throw std::invalid_argument("Format not specified");
    }
    return *it;
}
