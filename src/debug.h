/*
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

#include <cstddef>
#include <cstdint>

#include <linux/videodev2.h>

// Diagnostics, enabled by setting LIBVA_V4L2_DUMP=<existing directory>. The data of the first few pictures sent to
// the decoder is written to files named pic<N>.<what> there.
namespace debug {

// Call once per picture, when it begins.
void begin_picture();

// Writes a file for the current picture (does nothing if disabled, or for pictures past the first few).
void dump_file(const char* what, const void* data, size_t size);

// With LIBVA_V4L2_DUMP or LIBVA_V4L2_PROFILE set: print a backtrace to stderr if the process crashes with SIGSEGV or
// SIGABRT (addresses can be resolved with `addr2line -f -e v4l2_drv_video.so <offset>`).
void install_crash_handler();

// True if LIBVA_V4L2_QUIRKS (a comma separated list) contains `name`. Switches for experiments:
//   nocache - don't ask for cacheable capture buffers
//   rawpoc  - pass picture order counts as the application gives them (don't make them relative to the IDR)
bool quirk(const char* name);

// Remembers the VA parameters of the picture being decoded, so they can be dumped should it fail.
void remember_va_picture(const void* data, size_t size);

// Called when decoding a picture failed: writes fail<N>.source.bin / .controls.txt / .va_picture.bin (if enabled).
void dump_failure(const void* source, size_t size, const char* reason);

// With LIBVA_V4L2_PROFILE=1: collects timings, and prints averages (and maxima) every 60 samples.
void add_timing(const char* what, double milliseconds);

// `timestamp` is the one (in ns) the picture itself is queued with.
// With LIBVA_V4L2_TRACE=<file>, the description of every picture is appended to that file.
void dump_h264_controls(const v4l2_ctrl_h264_sps& sps, const v4l2_ctrl_h264_pps& pps,
    const v4l2_ctrl_h264_decode_params& decode, const v4l2_ctrl_h264_slice_params& slice, uint64_t timestamp);

}
