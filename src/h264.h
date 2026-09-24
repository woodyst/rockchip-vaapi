/*
 * h264.h — H.264 Annex B SPS/PPS reconstruction
 *
 * Copyright (C) 2026 Eduardo García-Mádico Portabella <woodyst@gmail.com>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once
#include <va/va.h>
#include <stdint.h>
#include <stddef.h>

int h264_write_sps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int profile_idc);

int h264_write_pps(uint8_t *buf, size_t buf_size,
                   const VAPictureParameterBufferH264 *pp,
                   int l0_default_minus1, int l1_default_minus1);

/* num_ref_idx_active_override_flag of a slice NAL (header byte included):
 * 0 or 1, or -1 for I/SI slices, non-slice NALs and parse errors. */
int h264_slice_ref_override(const uint8_t *nal, size_t size,
                            const VAPictureParameterBufferH264 *pp);
