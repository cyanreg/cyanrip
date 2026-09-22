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

/* Size of reads of audio + subchannel Q data. 2352 bytes for audio + 16 bytes for subchannel Q */
#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ (CDIO_CD_FRAMESIZE_RAW + SUBQ_SIZE)

/* Size of reads of audio + raw P-W subchannel data. 2352 bytes for audio + 96 bytes of P-W */
#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW (CDIO_CD_FRAMESIZE_RAW + CDIO_CD_FRAMESIZE_SUB)

/* Which sub-channel data a read returns after the audio */
enum cyanrip_subchannel {
    /* The formatted Q sub-channel: 16 bytes, the 12 byte Q frame zero padded.
     * The drive has decoded it, and may hand back its last good frame for a
     * sector it couldn't decode. Some drives leave the CRC out. */
    CYANRIP_SUBCHANNEL_Q = 0,
    /* The raw P-W sub-channel: the 96 subcode symbols as read off the disc,
     * bit 7 of each being the P channel, bit 6 the Q channel and so on down
     * to bit 0 for W. The drive neither checks the Q CRC nor substitutes
     * anything, so what comes back is what the sector holds. */
    CYANRIP_SUBCHANNEL_PW_RAW = 1,
};

/**
 * Reads a sector's audio followed by the given sub-channel data from a CD
 * device into buf. block_size is the number of bytes that makes, i.e.
 * CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ or CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW,
 * and buf must hold that many.
 *
 * Note: the Q frame still needs to be verified for CRC validity after reading.
 *
 * Returns DRIVER_OP_UNSUPPORTED where the backend can't read that sub-channel.
 */
driver_return_code_t cyanrip_read_audio_subchannel_sector(const CdIo_t *p_cdio, uint8_t *buf, const lsn_t lsn,
                                                          enum cyanrip_subchannel subchannel, size_t block_size);

/* ---- Reading and making sense of Q frames, on top of the backends above ---- */

/* Sectors read to find out whether the drive's raw P-W sub-channel is usable */
#define SUBQ_PROBE_SECTORS 10

/*
 * The maximum number of retries for a single sector read before giving up on that sector.
 * Based on XLD's pregap search, which uses 5 retries per sector.
 *
 * We might want to make this configurable in the future.
*/
#define SECTOR_MAX_RETRIES 5

/* Overall budget on how many failed (CRC-invalid) reads we'll tolerate
 * across the whole search before giving up entirely, so that severely
 * damaged media near a track boundary can't stall ripping indefinitely.
 * XLD's cap of 100 only counts failures before its first valid read; this
 * one covers the whole search.
 */
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

/* The sector a mode 1 Q frame says it belongs to, going by its absolute time */
static inline lsn_t subq_abs_lsn(const subq_t *subq)
{
    return (subq->amin * 60 + subq->asec) * 75 + subq->aframe - CDIO_PREGAP_SECTORS;
}

/**
 * Settles how the Q sub-channel is read for this drive (ctx->subq_read_mode),
 * by reading up to SUBQ_PROBE_SECTORS audio sectors raw from first_lsn, not
 * reaching end_lsn. Does nothing once settled. audio_subq_buf must hold
 * CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW bytes, as for the reads below.
 */
void subq_probe_read_mode(cyanrip_ctx *ctx, uint8_t *audio_subq_buf,
                          const lsn_t first_lsn, const lsn_t end_lsn);

/**
 * Reads and decodes the Q frame of a sector, retrying a CRC failure up to
 * SECTOR_MAX_RETRIES times and counting each failed attempt in
 * total_failures, which the caller checks against TOTAL_FAILURE_BUDGET.
 * Returns DRIVER_OP_SUCCESS with subq filled in, DRIVER_OP_ERROR for a frame
 * that never passed the CRC, or the backend's error.
 */
driver_return_code_t subq_read_with_retries(cyanrip_ctx *ctx, uint8_t *audio_subq_buf,
                                            subq_t *subq, const lsn_t lsn, int *total_failures);

/**
 * Last resort for a sector whose frame fails the CRC: repairs a single bit
 * error, or takes the frame as is if its absolute time is exactly lsn, and
 * returns 1 with subq filled in if the frame then reports one of the two
 * given tracks. *repaired says whether the CRC agrees after the repair.
 * Raw P-W mode only.
 */
int subq_read_damaged(cyanrip_ctx *ctx, uint8_t *audio_subq_buf, subq_t *subq,
                      const lsn_t lsn, const track_t prev_track_number,
                      const track_t track_number, int *repaired);
