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

#pragma once

#include <array>
#include <vector>

#include "v4l2.h"

struct LogicalPlane {
    unsigned physical_plane_index;
    unsigned size;
    unsigned pitch;
    unsigned offset;
};
using BufferLayout = std::vector<LogicalPlane>;

// Software conversion that has to be applied to a V4L2 capture buffer before it can be exposed as VA surface.
enum class Conversion {
    None,
    MM21ToNV12, // MediaTek 16x32 tiled NV12 (e.g. MT8183), see convert.h
};

struct Format {
    struct {
        fourcc format;
        BufferLayout (*derive_layout)(unsigned, unsigned);
        Conversion conversion;
    } v4l2;
    struct {
        fourcc format;
        uint32_t rt_format;
    } va;
    struct {
        fourcc format;
        uint64_t modifier;
    } drm;
};

extern const std::array<Format, 3> formats;

// Result of configuring the capture queue of a device for a render target format.
struct CaptureSetup {
    const Format* format;
    BufferLayout linear_layout; // target layout of converted formats, otherwise empty
    unsigned buffer_count; // buffers actually granted by the driver
    bool cpu_cached; // whether cacheable capture buffers were asked for
};

// Layout of a linear NV12 buffer with the given pitch (in bytes) and number of luma rows.
BufferLayout nv12_layout_with_pitch(unsigned pitch, unsigned height);
const Format& lookup_format(fourcc v4l2_fourcc);
