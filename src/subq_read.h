/*
 * This file is part of cyanrip.
 *
 * cyanrip is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * cyanrip is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with cyanrip; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <cdio/cdio.h>
#include "cyanrip_main.h"

#define SUBQ_SIZE 16

#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ (CDIO_CD_FRAMESIZE_RAW + SUBQ_SIZE)
#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW (CDIO_CD_FRAMESIZE_RAW + CDIO_CD_FRAMESIZE_SUB)

enum cyanrip_subchannel {
    /* 16 bytes, may be the drive's last good frame and may lack the CRC */
    CYANRIP_SUBCHANNEL_Q = 0,
    /* 96 bytes as read off the disc */
    CYANRIP_SUBCHANNEL_PW_RAW = 1,
};

/* Reads audio followed by sub-channel data, block_size is
 * CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ or CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW */
driver_return_code_t cyanrip_read_audio_subchannel_sector(const CdIo_t *p_cdio, uint8_t *buf, const lsn_t lsn,
                                                          enum cyanrip_subchannel subchannel, size_t block_size);

#define SUBQ_PROBE_SECTORS 10

/* Same as XLD */
#define SECTOR_MAX_RETRIES 5

/* Failed reads allowed per track search */
#define TOTAL_FAILURE_BUDGET 100

typedef struct subq_t {
    uint8_t  control;
    uint8_t  adr;
    uint8_t  track_number;
    uint8_t  index_number;
    uint8_t  min;
    uint8_t  sec;
    uint8_t  frame;
    uint8_t  amin;
    uint8_t  asec;
    uint8_t  aframe;
    unsigned crc;
} subq_t;

static inline lsn_t subq_abs_lsn(const subq_t *subq)
{
    return (subq->amin * 60 + subq->asec) * 75 + subq->aframe - CDIO_PREGAP_SECTORS;
}

/* Uses raw P-W if most of the probed frames pass the CRC. audio_subq_buf
 * must hold CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW bytes for all functions below. */
void subq_probe_read_mode(cyanrip_ctx *ctx, uint8_t *audio_subq_buf,
                          const lsn_t first_lsn, const lsn_t end_lsn);

/* Returns DRIVER_OP_ERROR if the CRC never matched */
driver_return_code_t subq_read_with_retries(cyanrip_ctx *ctx, uint8_t *audio_subq_buf,
                                            subq_t *subq, const lsn_t lsn, int *total_failures);

/* Repairs a single bit error, or accepts the frame if its absolute time is
 * exactly lsn. Returns 1 if it reports one of the two tracks. */
int subq_read_damaged(cyanrip_ctx *ctx, uint8_t *audio_subq_buf, subq_t *subq,
                      const lsn_t lsn, const track_t prev_track_number,
                      const track_t track_number, int *repaired);
