/*
 * h264.c — H.264 Annex B SPS/PPS reconstruction from VA-API structs
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "h264.h"
#include "bs.h"
#include <string.h>
#include <stdint.h>

/* Insert emulation prevention bytes (0x03) to avoid 0x000001/0x000002 sequences */
static size_t emulation_prevent(const uint8_t *in, size_t in_sz,
                                uint8_t *out, size_t out_cap) {
    size_t out_pos = 0;
    int zeros = 0;
    for (size_t i = 0; i < in_sz; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            if (out_pos >= out_cap) return 0;
            out[out_pos++] = 0x03;
            zeros = 0;
        }
        if (out_pos >= out_cap) return 0;
        out[out_pos++] = in[i];
        zeros = (in[i] == 0) ? zeros + 1 : 0;
    }
    return out_pos;
}

int h264_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int profile_idc)
{
    uint8_t raw[512];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));

    /* NAL header: forbidden_zero=0, nal_ref_idc=3, nal_unit_type=7 */
    bs_write(&bs, 0x67, 8);

    bs_write(&bs, (uint32_t)profile_idc, 8);

    /* constraint_set flags (derive from profile) + reserved.
     * Must be exactly 8 bits: constraint_set0..5 (6) + reserved_zero_2bits (2). */
    int c0 = (profile_idc == 66) ? 1 : 0;
    int c1 = (profile_idc == 66 || profile_idc == 77) ? 1 : 0;
    bs_write(&bs, c0, 1);
    bs_write(&bs, c1, 1);
    bs_write(&bs, 0, 1); /* constraint_set2 */
    bs_write(&bs, 0, 5); /* constraint_set3..5 (3) + reserved_zero_2bits (2) */

    bs_write(&bs, 51, 8); /* level_idc = 5.1 (safe for all content) */

    bs_write_ue(&bs, 0); /* seq_parameter_set_id */

    /* High-profile family gets chroma/bit-depth fields */
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128) {
        int cfi = pp->seq_fields.bits.chroma_format_idc;
        bs_write_ue(&bs, (uint32_t)cfi);
        if (cfi == 3)
            bs_write(&bs, pp->seq_fields.bits.residual_colour_transform_flag, 1);
        bs_write_ue(&bs, pp->bit_depth_luma_minus8);
        bs_write_ue(&bs, pp->bit_depth_chroma_minus8);
        bs_write(&bs, 0, 1); /* qpprime_y_zero_transform_bypass_flag */
        bs_write(&bs, 0, 1); /* seq_scaling_matrix_present_flag = 0 */
    }

    bs_write_ue(&bs, pp->seq_fields.bits.log2_max_frame_num_minus4);

    int poc_type = pp->seq_fields.bits.pic_order_cnt_type;
    bs_write_ue(&bs, (uint32_t)poc_type);
    if (poc_type == 0) {
        bs_write_ue(&bs, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (poc_type == 1) {
        bs_write(&bs, pp->seq_fields.bits.delta_pic_order_always_zero_flag, 1);
        bs_write_se(&bs, 0); /* offset_for_non_ref_pic */
        bs_write_se(&bs, 0); /* offset_for_top_to_bottom_field */
        bs_write_ue(&bs, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
    }

    bs_write_ue(&bs, pp->num_ref_frames);
    bs_write(&bs, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag, 1);
    bs_write_ue(&bs, pp->picture_width_in_mbs_minus1);

    int fmo = pp->seq_fields.bits.frame_mbs_only_flag;
    uint32_t ph_units = fmo ? (uint32_t)pp->picture_height_in_mbs_minus1
                            : ((uint32_t)pp->picture_height_in_mbs_minus1 + 1) / 2 - 1;
    bs_write_ue(&bs, ph_units);
    bs_write(&bs, fmo, 1);
    if (!fmo)
        bs_write(&bs, pp->seq_fields.bits.mb_adaptive_frame_field_flag, 1);

    bs_write(&bs, pp->seq_fields.bits.direct_8x8_inference_flag, 1);
    bs_write(&bs, 0, 1); /* frame_cropping_flag = 0 */
    bs_write(&bs, 0, 1); /* vui_parameters_present_flag = 0 */

    bs_rbsp_trailing(&bs);

    size_t raw_sz = bs_bytes(&bs);
    if (4 + raw_sz * 2 > buf_size) return -1;

    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    size_t ep = emulation_prevent(raw, raw_sz, buf + 4, buf_size - 4);
    if (!ep) return -1;
    return (int)(4 + ep);
}

int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int l0_default_minus1, int l1_default_minus1)
{
    uint8_t raw[256];
    BSWriter bs;
    bs_init(&bs, raw, sizeof(raw));

    /* NAL header: forbidden_zero=0, nal_ref_idc=3, nal_unit_type=8 */
    bs_write(&bs, 0x68, 8);

    bs_write_ue(&bs, 0); /* pic_parameter_set_id */
    bs_write_ue(&bs, 0); /* seq_parameter_set_id */
    bs_write(&bs, pp->pic_fields.bits.entropy_coding_mode_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.pic_order_present_flag, 1);
    bs_write_ue(&bs, 0); /* num_slice_groups_minus1 = 0 */

    /* num_ref_idx_l0/l1_default_active_minus1 are not in the VA picture
     * params.  They matter: a slice with num_ref_idx_active_override_flag=0
     * uses them to parse its ref list modifications and pred_weight_table,
     * so a wrong value misparses the slice header.  The caller learns them
     * from non-overriding slices (see h264_slice_ref_override). */
    bs_write_ue(&bs, (uint32_t)l0_default_minus1);
    bs_write_ue(&bs, (uint32_t)l1_default_minus1);

    bs_write(&bs, pp->pic_fields.bits.weighted_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.weighted_bipred_idc, 2);
    bs_write_se(&bs, pp->pic_init_qp_minus26);
    bs_write_se(&bs, pp->pic_init_qs_minus26);
    bs_write_se(&bs, pp->chroma_qp_index_offset);
    bs_write(&bs, pp->pic_fields.bits.deblocking_filter_control_present_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.constrained_intra_pred_flag, 1);
    bs_write(&bs, pp->pic_fields.bits.redundant_pic_cnt_present_flag, 1);

    /* More-data present if high-profile extensions needed */
    if (pp->pic_fields.bits.transform_8x8_mode_flag ||
        pp->second_chroma_qp_index_offset != pp->chroma_qp_index_offset) {
        bs_write(&bs, pp->pic_fields.bits.transform_8x8_mode_flag, 1);
        bs_write(&bs, 0, 1); /* pic_scaling_matrix_present_flag = 0 */
        bs_write_se(&bs, pp->second_chroma_qp_index_offset);
    }

    bs_rbsp_trailing(&bs);

    size_t raw_sz = bs_bytes(&bs);
    if (4 + raw_sz * 2 > buf_size) return -1;

    buf[0] = 0x00; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0x01;
    size_t ep = emulation_prevent(raw, raw_sz, buf + 4, buf_size - 4);
    if (!ep) return -1;
    return (int)(4 + ep);
}

/* Minimal RBSP bit reader for the start of a slice header */
typedef struct {
    uint8_t buf[64];
    size_t  len, pos;   /* pos in bits */
    int     err;
} BSReader;

static void br_init(BSReader *br, const uint8_t *nal, size_t size) {
    int zeros = 0;
    br->len = br->pos = 0;
    br->err = 0;
    for (size_t i = 0; i < size && br->len < sizeof(br->buf); i++) {
        if (zeros >= 2 && nal[i] == 0x03) { zeros = 0; continue; }
        br->buf[br->len++] = nal[i];
        zeros = (nal[i] == 0) ? zeros + 1 : 0;
    }
}

static uint32_t br_bit(BSReader *br) {
    if (br->pos >= br->len * 8) { br->err = 1; return 0; }
    uint32_t b = (br->buf[br->pos >> 3] >> (7 - (br->pos & 7))) & 1;
    br->pos++;
    return b;
}

static uint32_t br_bits(BSReader *br, int n) {
    uint32_t v = 0;
    while (n--) v = (v << 1) | br_bit(br);
    return v;
}

static uint32_t br_ue(BSReader *br) {
    int lz = 0;
    while (!br_bit(br) && !br->err && lz < 32) lz++;
    if (lz >= 32) { br->err = 1; return 0; }
    return ((1u << lz) - 1) + br_bits(br, lz);
}

static void br_se(BSReader *br) { (void)br_ue(br); }

int h264_slice_ref_override(const uint8_t *nal, size_t size,
                            const VAPictureParameterBufferH264 *pp)
{
    if (!nal || size < 2) return -1;
    int nal_type = nal[0] & 0x1F;
    if (nal_type != 1 && nal_type != 5) return -1;

    BSReader br;
    br_init(&br, nal + 1, size - 1);

    br_ue(&br);                                   /* first_mb_in_slice */
    int slice_type = (int)(br_ue(&br) % 5);
    if (slice_type == 2 || slice_type == 4) return -1;   /* I / SI */
    br_ue(&br);                                   /* pic_parameter_set_id */
    br_bits(&br, pp->seq_fields.bits.log2_max_frame_num_minus4 + 4);

    int field_pic = 0;
    if (!pp->seq_fields.bits.frame_mbs_only_flag) {
        field_pic = (int)br_bit(&br);
        if (field_pic) br_bit(&br);               /* bottom_field_flag */
    }
    if (nal_type == 5) br_ue(&br);                /* idr_pic_id */

    int poc_type = pp->seq_fields.bits.pic_order_cnt_type;
    int bottom_poc = pp->pic_fields.bits.pic_order_present_flag && !field_pic;
    if (poc_type == 0) {
        br_bits(&br, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (bottom_poc) br_se(&br);               /* delta_pic_order_cnt_bottom */
    } else if (poc_type == 1 &&
               !pp->seq_fields.bits.delta_pic_order_always_zero_flag) {
        br_se(&br);                               /* delta_pic_order_cnt[0] */
        if (bottom_poc) br_se(&br);               /* delta_pic_order_cnt[1] */
    }
    if (pp->pic_fields.bits.redundant_pic_cnt_present_flag)
        br_ue(&br);                               /* redundant_pic_cnt */
    if (slice_type == 1) br_bit(&br);             /* direct_spatial_mv_pred_flag */

    int override = (int)br_bit(&br);
    return br.err ? -1 : override;
}
