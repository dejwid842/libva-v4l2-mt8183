/*
 * Copyright (C) 2007 Intel Corporation
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
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

#include "h264.h"
#include "debug.h"
#include "linux/v4l2-controls.h"

#include <cassert>
#include <climits>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

extern "C" {
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <va/va.h>
}

#include "buffer.h"
#include "driver.h"
#include "surface.h"
#include "v4l2.h"

enum h264_slice_type {
    H264_SLICE_P = 0,
    H264_SLICE_B = 1,
};

enum h264_profile {
    H264_PROFILE_BASELINE = 66,
    H264_PROFILE_MAIN = 77,
    H264_PROFILE_SCALABLE_BASELINE = 83,
    H264_PROFILE_SCALABLE_HIGH = 86,
    H264_PROFILE_EXTENDED = 88,
    H264_PROFILE_HIGH = 100,
    H264_PROFILE_HIGH10 = 110,
    H264_PROFILE_HIGH_422 = 122,
    H264_PROFILE_MULTIVIEW_HIGH = 118,
    H264_PROFILE_STEREO_HIGH = 128,
    H264_PROFILE_HIGH_444 = 244,
};

namespace {

uint8_t va_profile_to_profile_idc(VAProfile profile)
{
    switch (profile) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
        return H264_PROFILE_BASELINE;
    case VAProfileH264High:
        return H264_PROFILE_HIGH;
    case VAProfileH264Main:
        return H264_PROFILE_MAIN;
    case VAProfileH264MultiviewHigh:
        return H264_PROFILE_MULTIVIEW_HIGH;
    case VAProfileH264StereoHigh:
        return H264_PROFILE_STEREO_HIGH;
    default:
        return 0;
    }
}

bool is_picture_null(VAPictureH264* pic)
{
    return pic->picture_id == VA_INVALID_SURFACE;
}

h264_dpb_entry* dpb_find_invalid_entry(H264Context& context)
{
    unsigned int i;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        if (!entry->valid && !entry->reserved)
            return entry;
    }

    return NULL;
}

h264_dpb_entry* dpb_find_oldest_unused_entry(H264Context& context)
{
    unsigned int min_age = UINT_MAX;
    unsigned int i;
    h264_dpb_entry* match = NULL;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        if (!entry->used && (entry->age < min_age)) {
            min_age = entry->age;
            match = entry;
        }
    }

    return match;
}

h264_dpb_entry* dpb_find_entry(H264Context& context)
{
    h264_dpb_entry* entry;

    entry = dpb_find_invalid_entry(context);
    if (!entry)
        entry = dpb_find_oldest_unused_entry(context);

    if (!entry) {
        // Everything is in use (more references than the DPB can hold?): take the oldest, rather than crash.
        entry = &context.dpb.entries[0];
        for (auto& candidate : context.dpb.entries) {
            if (candidate.age < entry->age)
                entry = &candidate;
        }
    }

    return entry;
}

h264_dpb_entry* dpb_lookup(H264Context& context, VAPictureH264* pic, v4l2_h264_reference* ref)
{
    unsigned int i;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        if (!entry->valid)
            continue;

        if (entry->pic.picture_id == pic->picture_id) {
            if (ref) {
                ref->index = i;
                if (pic->flags & VA_PICTURE_H264_BOTTOM_FIELD) {
                    ref->fields |= V4L2_H264_BOTTOM_FIELD_REF;
                }
                if (pic->flags & VA_PICTURE_H264_TOP_FIELD) {
                    ref->fields |= V4L2_H264_TOP_FIELD_REF;
                }
            }

            return entry;
        }
    }

    return NULL;
}

void dpb_clear_entry(h264_dpb_entry* entry, bool reserved)
{
    memset(entry, 0, sizeof(*entry));

    if (reserved)
        entry->reserved = true;
}

void dpb_insert(H264Context& context, VAPictureH264* pic, h264_dpb_entry* entry)
{
    if (is_picture_null(pic))
        return;

    if (dpb_lookup(context, pic, NULL))
        return;

    if (!entry)
        entry = dpb_find_entry(context);

    memcpy(&entry->pic, pic, sizeof(entry->pic));
    entry->age = context.dpb.age;
    entry->valid = true;
    entry->reserved = false;

    if (!(pic->flags & VA_PICTURE_H264_INVALID))
        entry->used = true;
}

void dpb_update(H264Context& context, VAPictureParameterBufferH264* parameters)
{
    unsigned int i;
    context.dpb.age++;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        h264_dpb_entry* entry = &context.dpb.entries[i];

        entry->used = false;
    }

    for (i = 0; i < parameters->num_ref_frames; i++) {
        VAPictureH264* pic = &parameters->ReferenceFrames[i];
        h264_dpb_entry* entry;

        if (is_picture_null(pic) || (pic->flags & VA_PICTURE_H264_INVALID)
            || pic->picture_id == parameters->CurrPic.picture_id)
            continue;

        entry = dpb_lookup(context, pic, NULL);
        if (entry) {
            // Keep what is known about the picture current (e.g. it may have become a long term reference).
            memcpy(&entry->pic, pic, sizeof(entry->pic));
            entry->age = context.dpb.age;
            entry->used = true;
        } else {
            dpb_insert(context, pic, NULL);
        }
    }
}

void h264_fill_dpb(DriverData* data, H264Context& context, v4l2_ctrl_h264_decode_params* decode,
    VASurfaceID current_picture, unsigned current_frame_num, unsigned max_frame_num)
{
    int i;

    for (i = 0; i < H264_DPB_SIZE; i++) {
        v4l2_h264_dpb_entry* dpb = &decode->dpb[i];
        h264_dpb_entry* entry = &context.dpb.entries[i];

        const auto& surface = data->surfaces.find(entry->pic.picture_id);

        uint64_t timestamp;

        if (!entry->valid)
            continue;

        // The picture being decoded is not a reference of itself: listing it would make the decoder both read
        // and write the same buffer.
        if (entry->pic.picture_id == current_picture)
            continue;

        // Without a surface there's no buffer (timestamp) to refer to.
        if (surface == data->surfaces.end())
            continue;

        timestamp = v4l2_timeval_to_ns(&surface->second.timestamp);
        dpb->reference_ts = timestamp;

        dpb->frame_num = entry->pic.frame_idx;
        dpb->top_field_order_cnt = entry->pic.TopFieldOrderCnt - context.poc_base;
        dpb->bottom_field_order_cnt = entry->pic.BottomFieldOrderCnt - context.poc_base;

        // Which fields of the entry can be referenced; decoders (and the kernel's reference list builder) rely on it,
        // so it can't be left at 0 for ordinary frames.
        if (entry->pic.flags & VA_PICTURE_H264_TOP_FIELD)
            dpb->fields = V4L2_H264_TOP_FIELD_REF;
        else if (entry->pic.flags & VA_PICTURE_H264_BOTTOM_FIELD)
            dpb->fields = V4L2_H264_BOTTOM_FIELD_REF;
        else
            dpb->fields = V4L2_H264_FRAME_REF;

        // PicNum of short term pictures is FrameNumWrap (7.4.3), and LongTermPicNum of long term ones is
        // LongTermFrameIdx (both for frames, which is all that's handled here).
        if (entry->pic.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
            dpb->pic_num = entry->pic.frame_idx;
        else
            dpb->pic_num = (entry->pic.frame_idx > current_frame_num) ? entry->pic.frame_idx - max_frame_num
                                                                     : entry->pic.frame_idx;

        dpb->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID;

        if (entry->used)
            dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;

        if (entry->pic.flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
            dpb->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
    }
}

// VA-API doesn't carry the level of the stream, but decoders (at least MediaTek's) may size their internal buffers
// by it. Pick the lowest level that can hold the picture size and the number of reference frames.
uint8_t h264_guess_level_idc(unsigned width_in_mbs, unsigned height_in_mbs, unsigned num_ref_frames)
{
    static const struct {
        uint8_t level_idc;
        unsigned max_frame_size_mbs;
        unsigned max_dpb_mbs;
    } levels[] = {
        { 10, 99, 396 }, { 11, 396, 900 }, { 12, 396, 2376 }, { 20, 396, 2376 }, { 21, 792, 4752 },
        { 22, 1620, 8100 }, { 30, 1620, 8100 }, { 31, 3600, 18000 }, { 32, 5120, 20480 }, { 40, 8192, 32768 },
        { 42, 8704, 34816 }, { 50, 22080, 110400 }, { 51, 36864, 184320 },
    };

    const unsigned frame_size = width_in_mbs * height_in_mbs;
    for (const auto& level : levels) {
        if (frame_size <= level.max_frame_size_mbs && frame_size * num_ref_frames <= level.max_dpb_mbs)
            return level.level_idc;
    }
    return 52;
}

void h264_va_picture_to_v4l2(DriverData* driver_data, H264Context& context, VAPictureParameterBufferH264* VAPicture,
    v4l2_ctrl_h264_decode_params* decode, v4l2_ctrl_h264_pps* pps, v4l2_ctrl_h264_sps* sps)
{
    h264_fill_dpb(driver_data, context, decode, VAPicture->CurrPic.picture_id, VAPicture->frame_num,
        1u << (VAPicture->seq_fields.bits.log2_max_frame_num_minus4 + 4));

    decode->frame_num = VAPicture->frame_num;
    if (VAPicture->pic_fields.bits.field_pic_flag) {
        decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
        if (VAPicture->CurrPic.flags & VA_PICTURE_H264_BOTTOM_FIELD)
            decode->flags |= V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD;
    }

    decode->top_field_order_cnt = VAPicture->CurrPic.TopFieldOrderCnt - context.poc_base;
    decode->bottom_field_order_cnt = VAPicture->CurrPic.BottomFieldOrderCnt - context.poc_base;

    pps->weighted_bipred_idc = VAPicture->pic_fields.bits.weighted_bipred_idc;
    pps->pic_init_qs_minus26 = VAPicture->pic_init_qs_minus26;
    pps->pic_init_qp_minus26 = VAPicture->pic_init_qp_minus26;
    pps->chroma_qp_index_offset = VAPicture->chroma_qp_index_offset;
    pps->second_chroma_qp_index_offset = VAPicture->second_chroma_qp_index_offset;

    if (VAPicture->pic_fields.bits.entropy_coding_mode_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

    if (VAPicture->pic_fields.bits.weighted_pred_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;

    if (VAPicture->pic_fields.bits.transform_8x8_mode_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

    if (VAPicture->pic_fields.bits.constrained_intra_pred_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;

    if (VAPicture->pic_fields.bits.pic_order_present_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;

    if (VAPicture->pic_fields.bits.deblocking_filter_control_present_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

    if (VAPicture->pic_fields.bits.redundant_pic_cnt_present_flag)
        pps->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;

    sps->chroma_format_idc = VAPicture->seq_fields.bits.chroma_format_idc;
    sps->bit_depth_luma_minus8 = VAPicture->bit_depth_luma_minus8;
    sps->bit_depth_chroma_minus8 = VAPicture->bit_depth_chroma_minus8;
    sps->log2_max_frame_num_minus4 = VAPicture->seq_fields.bits.log2_max_frame_num_minus4;
    sps->pic_order_cnt_type = VAPicture->seq_fields.bits.pic_order_cnt_type;
    sps->log2_max_pic_order_cnt_lsb_minus4 = VAPicture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
    sps->max_num_ref_frames = VAPicture->num_ref_frames;
    sps->pic_width_in_mbs_minus1 = VAPicture->picture_width_in_mbs_minus1;
    sps->pic_height_in_map_units_minus1 = VAPicture->picture_height_in_mbs_minus1;
    sps->level_idc = h264_guess_level_idc(VAPicture->picture_width_in_mbs_minus1 + 1,
        VAPicture->picture_height_in_mbs_minus1 + 1, VAPicture->num_ref_frames);

    if (VAPicture->seq_fields.bits.residual_colour_transform_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
    if (VAPicture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
    if (VAPicture->seq_fields.bits.frame_mbs_only_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
    if (VAPicture->seq_fields.bits.mb_adaptive_frame_field_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
    if (VAPicture->seq_fields.bits.direct_8x8_inference_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
    if (VAPicture->seq_fields.bits.delta_pic_order_always_zero_flag)
        sps->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
}

void h264_va_matrix_to_v4l2(DriverData* driver_data, const H264Context& context, VAIQMatrixBufferH264* VAMatrix,
    v4l2_ctrl_h264_scaling_matrix* v4l2_matrix)
{
    // Lists the stream doesn't have (VA-API only gives the luma ones for 8x8) must not be left at zero: decoders may
    // use them regardless of what the stream says. Flat is what the kernel starts with, too.
    memset(v4l2_matrix, 16, sizeof(*v4l2_matrix));

    // 4x4: Intra Y, Intra Cb, Intra Cr, Inter Y, Inter Cb, Inter Cr, which is the order VA-API uses as well.
    memcpy(v4l2_matrix->scaling_list_4x4, &VAMatrix->ScalingList4x4, sizeof(VAMatrix->ScalingList4x4));

    /*
     * 8x8 has a different order (H.264 table 7-2): Intra Y, Inter Y, Intra Cb, Inter Cb, Intra Cr, Inter Cr. Only
     * the first two are involved unless the stream is 4:4:4, and those are what VA-API gives.
     * (Placing Inter Y at index 3, like the earlier V4L2 API wanted, leaves a zero list where decoders look for it,
     * which wipes out the residual of every inter coded 8x8 transform block.)
     */
    memcpy(v4l2_matrix->scaling_list_8x8[0], &VAMatrix->ScalingList8x8[0], sizeof(v4l2_matrix->scaling_list_8x8[0]));
    memcpy(v4l2_matrix->scaling_list_8x8[1], &VAMatrix->ScalingList8x8[1], sizeof(v4l2_matrix->scaling_list_8x8[1]));
}

void h264_copy_pred_table(v4l2_h264_weight_factors* factors, unsigned int num_refs, int16_t luma_weight[32],
    int16_t luma_offset[32], int16_t chroma_weight[32][2], int16_t chroma_offset[32][2])
{
    unsigned int i;

    for (i = 0; i < num_refs; i++) {
        unsigned int j;

        factors->luma_weight[i] = luma_weight[i];
        factors->luma_offset[i] = luma_offset[i];

        for (j = 0; j < 2; j++) {
            factors->chroma_weight[i][j] = chroma_weight[i][j];
            factors->chroma_offset[i][j] = chroma_offset[i][j];
        }
    }
}

void h264_va_slice_to_v4l2(DriverData* driver_data, H264Context& context, VASliceParameterBufferH264* VASlice,
    VAPictureParameterBufferH264* VAPicture, v4l2_ctrl_h264_slice_params* slice)
{
    slice->header_bit_size = VASlice->slice_data_bit_offset;
    slice->first_mb_in_slice = VASlice->first_mb_in_slice;
    slice->slice_type = VASlice->slice_type;
    slice->cabac_init_idc = VASlice->cabac_init_idc;
    slice->slice_qp_delta = VASlice->slice_qp_delta;
    slice->disable_deblocking_filter_idc = VASlice->disable_deblocking_filter_idc;
    slice->slice_alpha_c0_offset_div2 = VASlice->slice_alpha_c0_offset_div2;
    slice->slice_beta_offset_div2 = VASlice->slice_beta_offset_div2;

    if (((VASlice->slice_type % 5) == H264_SLICE_P) || ((VASlice->slice_type % 5) == H264_SLICE_B)) {
        slice->num_ref_idx_l0_active_minus1 = VASlice->num_ref_idx_l0_active_minus1;

        for (int i = 0; i < VASlice->num_ref_idx_l0_active_minus1 + 1; i++) {
            VAPictureH264* pic = &VASlice->RefPicList0[i];
            h264_dpb_entry* entry;
            v4l2_h264_reference ref = {};

            entry = dpb_lookup(context, pic, &ref);
            if (!entry)
                continue;

            slice->ref_pic_list0[i] = ref;
        }
    }

    if ((VASlice->slice_type % 5) == H264_SLICE_B) {
        slice->num_ref_idx_l1_active_minus1 = VASlice->num_ref_idx_l1_active_minus1;

        for (int i = 0; i < VASlice->num_ref_idx_l1_active_minus1 + 1; i++) {
            VAPictureH264* pic = &VASlice->RefPicList1[i];
            h264_dpb_entry* entry;
            v4l2_h264_reference ref = {};

            entry = dpb_lookup(context, pic, &ref);
            if (!entry)
                continue;

            slice->ref_pic_list1[i] = ref;
        }
    }

    if (VASlice->direct_spatial_mv_pred_flag)
        slice->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
}

void h264_va_slice_to_predicted_weights(
    VASliceParameterBufferH264* VASlice, v4l2_ctrl_h264_slice_params* slice, v4l2_ctrl_h264_pred_weights* weights)
{

    weights->chroma_log2_weight_denom = VASlice->chroma_log2_weight_denom;
    weights->luma_log2_weight_denom = VASlice->luma_log2_weight_denom;

    if (((VASlice->slice_type % 5) == H264_SLICE_P) || ((VASlice->slice_type % 5) == H264_SLICE_B))
        h264_copy_pred_table(&weights->weight_factors[0], slice->num_ref_idx_l0_active_minus1 + 1,
            VASlice->luma_weight_l0, VASlice->luma_offset_l0, VASlice->chroma_weight_l0, VASlice->chroma_offset_l0);

    if ((VASlice->slice_type % 5) == H264_SLICE_B)
        h264_copy_pred_table(&weights->weight_factors[1], slice->num_ref_idx_l1_active_minus1 + 1,
            VASlice->luma_weight_l1, VASlice->luma_offset_l1, VASlice->chroma_weight_l1, VASlice->chroma_offset_l1);
}

} // namespace

H264Context::H264Context(DriverData* driver_data, V4L2M2MDevice& device, VAProfile profile, int picture_width,
    int picture_height, std::span<VASurfaceID> surface_ids)
    : Context(driver_data, device, V4L2_PIX_FMT_H264_SLICE, picture_width, picture_height, surface_ids)
    , profile(va_profile_to_profile_idc(profile))
    , mode(static_cast<v4l2_stateless_h264_decode_mode>(device.get_control(V4L2_CID_STATELESS_H264_DECODE_MODE)))
{
}

VAStatus H264Context::store_buffer(const Buffer& buffer) const
{
    auto& surface = driver_data->surfaces.at(render_surface_id);

    const auto source_data = surface.source_buffer->get().mapping()[0];
    switch (buffer.type) {
    case VASliceDataBufferType:
    {
        const bool start_code
            = mode == static_cast<v4l2_stateless_h264_decode_mode>(V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED);

        // Check first, so that nothing is ever written past the end of the buffer.
        if (surface.source_size_used + (start_code ? 3 : 0) + buffer.size * buffer.count > source_data.size()) {
            return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
        }

        if (start_code) {
            surface.source_size_used = std::ranges::copy(std::initializer_list<uint8_t>{0, 0, 1}, source_data.data() + surface.source_size_used).out - source_data.data();
        }
        memcpy(source_data.data() + surface.source_size_used, buffer.data.get(), buffer.size * buffer.count);
        surface.source_size_used += buffer.size * buffer.count;
    }
        break;

    case VAPictureParameterBufferType:
        surface.params.h264.picture = reinterpret_cast<VAPictureParameterBufferH264*>(buffer.data.get());
        break;

    case VASliceParameterBufferType:
        surface.params.h264.slice = reinterpret_cast<VASliceParameterBufferH264*>(buffer.data.get());
        break;

    case VAIQMatrixBufferType:
        surface.params.h264.matrix = reinterpret_cast<VAIQMatrixBufferH264*>(buffer.data.get());
        break;

    default:
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }

    return VA_STATUS_SUCCESS;
}

int H264Context::set_controls()
{
    auto& surface = driver_data->surfaces.at(render_surface_id);

    // Without these there's nothing to decode (the application, or an earlier step, failed to deliver them).
    if (!surface.params.h264.picture || !surface.params.h264.slice || surface.source_size_used == 0) {
        fprintf(stderr, "libva-v4l2: incomplete H.264 picture (picture parameters %p, slice parameters %p, %u bytes)\n",
            static_cast<void*>(surface.params.h264.picture), static_cast<void*>(surface.params.h264.slice),
            surface.source_size_used);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    v4l2_ctrl_h264_scaling_matrix matrix = {};
    v4l2_ctrl_h264_decode_params decode = {};
    v4l2_ctrl_h264_slice_params slice = {};
    v4l2_ctrl_h264_pps pps = {};
    v4l2_ctrl_h264_sps sps = {};
    h264_dpb_entry* output;

    // nal_ref_idc and the IDR flag are not part of the VA-API parameters, but are in the NAL unit header, which is
    // the first byte after the start code (if it is there).
    uint8_t nal_header = 0;
    {
        const auto source = surface.source_buffer->get().mapping()[0];
        if (surface.source_size_used > 3 && source[0] == 0 && source[1] == 0 && source[2] == 1)
            nal_header = source[3];
        else if (surface.source_size_used > 0)
            nal_header = source[0];
    }
    const bool idr_picture = (nal_header & 0x1f) == 5;

    // An IDR picture invalidates all earlier references.
    if (idr_picture) {
        for (auto& entry : dpb.entries)
            dpb_clear_entry(&entry, false);
    }

    // Order counts relative to the IDR picture starting the sequence (or the first picture seen).
    if (debug::quirk("rawpoc")) {
        poc_base = 0;
    } else if (idr_picture || !poc_base_valid) {
        const auto& current = surface.params.h264.picture->CurrPic;
        poc_base = std::min(current.TopFieldOrderCnt, current.BottomFieldOrderCnt);
        poc_base_valid = true;
    }

    debug::dump_file("va_picture.bin", surface.params.h264.picture, sizeof(*surface.params.h264.picture));
    debug::remember_va_picture(surface.params.h264.picture, sizeof(*surface.params.h264.picture));

    output = dpb_lookup(*this, &surface.params.h264.picture->CurrPic, NULL);
    if (!output)
        output = dpb_find_entry(*this);

    dpb_clear_entry(output, true);

    dpb_update(*this, surface.params.h264.picture);

    h264_va_picture_to_v4l2(driver_data, *this, surface.params.h264.picture, &decode, &pps, &sps);
    if (surface.params.h264.matrix) {
        h264_va_matrix_to_v4l2(driver_data, *this, surface.params.h264.matrix, &matrix);
    } else {
        // No matrices given: flat ones, which is what the stream specifies without any.
        memset(&matrix, 16, sizeof(matrix));
    }

    // The default number of active references of the PPS is not known (VA-API doesn't pass it on), but decoders that
    // parse the slice header themselves need it whenever the slice doesn't override it, in which case it's equal
    // to the slice's. When the slice does override it, the value isn't looked at. So the slice's count is right either way.
    switch (surface.params.h264.slice->slice_type % 5) {
    case H264_SLICE_B:
        pps.num_ref_idx_l1_default_active_minus1 = surface.params.h264.slice->num_ref_idx_l1_active_minus1;
        [[fallthrough]];
    case H264_SLICE_P:
        pps.num_ref_idx_l0_default_active_minus1 = surface.params.h264.slice->num_ref_idx_l0_active_minus1;
        break;
    }
    h264_va_slice_to_v4l2(driver_data, *this, surface.params.h264.slice, surface.params.h264.picture, &slice);

    sps.profile_idc = profile;
    decode.nal_ref_idc = (nal_header >> 5) & 3;
    if (idr_picture)
        decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
    switch (surface.params.h264.slice->slice_type % 5) {
    case H264_SLICE_P:
        decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
        break;
    case H264_SLICE_B:
        decode.flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
        break;
    }
    debug::dump_h264_controls(sps, pps, decode, slice, v4l2_timeval_to_ns(&surface.timestamp));

    std::vector<v4l2_ext_control> controls = {
        {
            .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
            .size = sizeof(decode),
            .ptr = &decode,
        },

        {
            .id = V4L2_CID_STATELESS_H264_PPS,
            .size = sizeof(pps),
            .ptr = &pps,
        },

        {
            .id = V4L2_CID_STATELESS_H264_SPS,
            .size = sizeof(sps),
            .ptr = &sps,
        },
        {
            .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
            .size = sizeof(matrix),
            .ptr = &matrix,
        },
    };

    if (mode == static_cast<v4l2_stateless_h264_decode_mode>(V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED)) {
        controls.push_back({
            .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
            .size = sizeof(slice),
            .ptr = &slice,
        });
    }

    // Frame based decoders (e.g. MediaTek, Rockchip, Hantro) parse the slice headers themselves, and don't
    // implement the slice params / prediction weights controls, so they must not be set for them.
    v4l2_ctrl_h264_pred_weights weights = {};
    if (mode == static_cast<v4l2_stateless_h264_decode_mode>(V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED)
        && V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(&pps, &slice)) {
        h264_va_slice_to_predicted_weights(surface.params.h264.slice, &slice, &weights);

        controls.push_back({
            .id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS,
            .size = sizeof(weights),
            .ptr = &weights,
        });
    }

    try {
        device.set_ext_controls(surface.request_fd, std::span(controls));
    } catch (std::runtime_error& e) {
        fprintf(stderr, "libva-v4l2: unable to set H.264 controls: %s\n", e.what());
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    dpb_insert(*this, &surface.params.h264.picture->CurrPic, output);

    return VA_STATUS_SUCCESS;
}

std::set<VAProfile> H264Context::supported_profiles(const V4L2M2MDevice& device)
{
    // TODO: query `h264_profile` control for more details
    return (device.format_supported(device.output_buf_type, V4L2_PIX_FMT_H264_SLICE))
        ? std::set<VAProfile>({ VAProfileH264Main, VAProfileH264High, VAProfileH264ConstrainedBaseline,
              VAProfileH264MultiviewHigh, VAProfileH264StereoHigh })
        : std::set<VAProfile>();
};
