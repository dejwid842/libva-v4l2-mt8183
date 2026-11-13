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

#include "debug.h"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace debug {

namespace {
    constexpr unsigned max_pictures = 4;
    constexpr unsigned max_failures = 8;
    int picture = -1;
    unsigned failures = 0;
    std::string last_controls;
    std::vector<uint8_t> last_va_picture;

    const char* directory()
    {
        const char* value = getenv("LIBVA_V4L2_DUMP");
        return (value && *value) ? value : nullptr;
    }

    void write_path(const std::string& name, const void* data, size_t size)
    {
        if (!directory()) {
            return;
        }
        const std::string path = std::string(directory()) + "/" + name;
        if (FILE* file = fopen(path.c_str(), "wb")) {
            fwrite(data, 1, size, file);
            fclose(file);
        }
    }

    void appendf(std::string& text, const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        text += buffer;
    }

    bool dump_this_picture() { return picture >= 0 && picture < static_cast<int>(max_pictures); }
}

void begin_picture() { picture++; }

bool quirk(const char* name)
{
    const char* list = getenv("LIBVA_V4L2_QUIRKS");
    if (!list) {
        return false;
    }
    const size_t length = strlen(name);
    for (const char* found = list; (found = strstr(found, name)); found += length) {
        const bool starts = found == list || found[-1] == ',';
        const bool ends = found[length] == '\0' || found[length] == ',';
        if (starts && ends) {
            return true;
        }
    }
    return false;
}

namespace {
    void crash_handler(int signal_number)
    {
        static const char message[] = "libva-v4l2: crashed, backtrace:\n";
        write(STDERR_FILENO, message, sizeof(message) - 1);
        void* frames[48];
        const int count = backtrace(frames, 48);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        // Let the system handle it as usual (core dump, shell message).
        signal(signal_number, SIG_DFL);
        raise(signal_number);
    }
}

void install_crash_handler()
{
    static bool installed = false;
    if (installed || !(directory() || getenv("LIBVA_V4L2_PROFILE"))) {
        return;
    }
    installed = true;
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
}

void dump_file(const char* what, const void* data, size_t size)
{
    if (dump_this_picture()) {
        write_path("pic" + std::to_string(picture) + "." + what, data, size);
    }
}

void remember_va_picture(const void* data, size_t size)
{
    if (!directory()) {
        return;
    }
    last_va_picture.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
}

void dump_failure(const void* source, size_t size, const char* reason)
{
    if (!directory() || failures >= max_failures) {
        return;
    }
    const std::string prefix = "fail" + std::to_string(failures++) + ".";
    const std::string controls = std::string("reason: ") + reason + "\npicture number: " + std::to_string(picture) + "\n"
        + last_controls;
    write_path(prefix + "source.bin", source, size);
    write_path(prefix + "controls.txt", controls.data(), controls.size());
    write_path(prefix + "va_picture.bin", last_va_picture.data(), last_va_picture.size());
}

void add_timing(const char* what, double milliseconds)
{
    static bool enabled = getenv("LIBVA_V4L2_PROFILE") != nullptr;
    if (!enabled) {
        return;
    }

    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);

    struct Stat {
        const char* what = nullptr;
        unsigned count = 0;
        double sum = 0, max = 0;
    };
    static Stat stats[8];

    for (auto& stat : stats) {
        if (stat.what && strcmp(stat.what, what) != 0) {
            continue;
        }
        stat.what = what;
        stat.count++;
        stat.sum += milliseconds;
        stat.max = milliseconds > stat.max ? milliseconds : stat.max;
        if (stat.count == 60) {
            fprintf(stderr, "libva-v4l2: %s: average %.1f ms, max %.1f ms (60 samples)\n", what, stat.sum / 60, stat.max);
            stat.count = 0;
            stat.sum = stat.max = 0;
        }
        return;
    }
}

void dump_h264_controls(const v4l2_ctrl_h264_sps& sps, const v4l2_ctrl_h264_pps& pps,
    const v4l2_ctrl_h264_decode_params& decode, const v4l2_ctrl_h264_slice_params& slice, uint64_t timestamp)
{
    const char* trace = getenv("LIBVA_V4L2_TRACE");
    if (!directory() && !(trace && *trace)) {
        return;
    }

    std::string text;
    appendf(text,
        "SPS: profile_idc %u constraint_set_flags 0x%x level_idc %u sps_id %u chroma_format_idc %u bit_depth_luma_minus8 %u "
        "bit_depth_chroma_minus8 %u log2_max_frame_num_minus4 %u pic_order_cnt_type %u log2_max_poc_lsb_minus4 %u "
        "max_num_ref_frames %u num_ref_frames_in_poc_cycle %u width_in_mbs_minus1 %u height_in_map_units_minus1 %u "
        "flags 0x%x\n",
        sps.profile_idc, sps.constraint_set_flags, sps.level_idc, sps.seq_parameter_set_id, sps.chroma_format_idc,
        sps.bit_depth_luma_minus8, sps.bit_depth_chroma_minus8, sps.log2_max_frame_num_minus4, sps.pic_order_cnt_type,
        sps.log2_max_pic_order_cnt_lsb_minus4, sps.max_num_ref_frames, sps.num_ref_frames_in_pic_order_cnt_cycle,
        sps.pic_width_in_mbs_minus1, sps.pic_height_in_map_units_minus1, sps.flags);

    appendf(text,
        "PPS: pps_id %u sps_id %u num_slice_groups_minus1 %u num_ref_idx_l0_default_minus1 %u "
        "num_ref_idx_l1_default_minus1 %u weighted_bipred_idc %u pic_init_qp_minus26 %d pic_init_qs_minus26 %d "
        "chroma_qp_index_offset %d second_chroma_qp_index_offset %d flags 0x%x\n",
        pps.pic_parameter_set_id, pps.seq_parameter_set_id, pps.num_slice_groups_minus1,
        pps.num_ref_idx_l0_default_active_minus1, pps.num_ref_idx_l1_default_active_minus1, pps.weighted_bipred_idc,
        pps.pic_init_qp_minus26, pps.pic_init_qs_minus26, pps.chroma_qp_index_offset,
        pps.second_chroma_qp_index_offset, pps.flags);

    appendf(text,
        "DECODE_PARAMS: nal_ref_idc %u frame_num %u top_poc %d bottom_poc %d idr_pic_id %u poc_lsb %u "
        "delta_poc_bottom %d delta_poc0 %d delta_poc1 %d dec_ref_pic_marking_bit_size %u pic_order_cnt_bit_size %u "
        "slice_group_change_cycle %u flags 0x%x\n",
        decode.nal_ref_idc, decode.frame_num, decode.top_field_order_cnt, decode.bottom_field_order_cnt,
        decode.idr_pic_id, decode.pic_order_cnt_lsb, decode.delta_pic_order_cnt_bottom, decode.delta_pic_order_cnt0,
        decode.delta_pic_order_cnt1, decode.dec_ref_pic_marking_bit_size, decode.pic_order_cnt_bit_size,
        decode.slice_group_change_cycle, decode.flags);

    for (unsigned i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
        const auto& entry = decode.dpb[i];
        if (entry.flags == 0 && entry.reference_ts == 0) {
            continue;
        }
        appendf(text, "DPB[%u]: reference_ts %llu pic_num %u frame_num %u fields %u top_poc %d bottom_poc %d flags 0x%x\n",
            i, static_cast<unsigned long long>(entry.reference_ts), entry.pic_num, entry.frame_num, entry.fields,
            entry.top_field_order_cnt, entry.bottom_field_order_cnt, entry.flags);
    }

    appendf(text,
        "SLICE (last one only): header_bit_size %u first_mb_in_slice %u slice_type %u colour_plane_id %u "
        "redundant_pic_cnt %u cabac_init_idc %u slice_qp_delta %d slice_qs_delta %d disable_deblocking_filter_idc %u "
        "slice_alpha_c0_offset_div2 %d slice_beta_offset_div2 %d num_ref_idx_l0_active_minus1 %u "
        "num_ref_idx_l1_active_minus1 %u flags 0x%x\n",
        slice.header_bit_size, slice.first_mb_in_slice, slice.slice_type, slice.colour_plane_id,
        slice.redundant_pic_cnt, slice.cabac_init_idc, slice.slice_qp_delta, slice.slice_qs_delta,
        slice.disable_deblocking_filter_idc, slice.slice_alpha_c0_offset_div2, slice.slice_beta_offset_div2,
        slice.num_ref_idx_l0_active_minus1, slice.num_ref_idx_l1_active_minus1, slice.flags);

    // The reference lists as the application (VA-API) gives them: indexes into the DPB entries above.
    const unsigned slice_type = slice.slice_type % 5;
    if (slice_type == 0 || slice_type == 1 || slice_type == 3) {
        appendf(text, "VA_LIST0:");
        for (unsigned i = 0; i <= slice.num_ref_idx_l0_active_minus1 && i < 32; i++)
            appendf(text, " %u", slice.ref_pic_list0[i].index);
        appendf(text, "\n");
    }
    if (slice_type == 1) {
        appendf(text, "VA_LIST1:");
        for (unsigned i = 0; i <= slice.num_ref_idx_l1_active_minus1 && i < 32; i++)
            appendf(text, " %u", slice.ref_pic_list1[i].index);
        appendf(text, "\n");
    }

    last_controls = text;
    if (trace && *trace) {
        if (FILE* file = fopen(trace, "ab")) {
            fprintf(file, "=== picture %d, timestamp %llu\n%s", picture, static_cast<unsigned long long>(timestamp),
                text.c_str());
            fclose(file);
        }
    }
    if (dump_this_picture()) {
        write_path("pic" + std::to_string(picture) + ".controls.txt", text.data(), text.size());
    }
}

}
