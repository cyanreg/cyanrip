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

#include "subq_read.h"
#include "pregap.h"
#include "cyanrip_log.h"

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include <cdio/cdio.h>
#include <cdio/mmc_ll_cmds.h>

/*
 * The maximum number of retries for a single sector read before giving up on that sector.
 * Based on XLD's pregap search, which uses 5 retries per sector.
 * 
 * We might want to make this configurable in the future.
*/
#define SECTOR_MAX_RETRIES 5

/* Overall budget on how many failed (CRC-invalid) reads we'll tolerate
 * across the whole search before giving up entirely, so that severely
 * damaged media near a track boundary can't stall ripping indefinitely
 * (XLD has an equivalent global cap).
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

static inline uint8_t bcd_to_bin(uint8_t x)
{
    return 10 * ((x & 0xF0) >> 4) + (x & 0x0F);
}

/* MMC-3 4.1.3.2.1. Q sub-channel Mode-1: "Bytes in the Q sub-channel that
 * contains bcd contents may also contain illegal BCD values. Then values start
 * with 0A0h and continue to 0FFh. No conversion of these to hex for
 * transmission to/from the initiator is performed."
 */
static inline uint8_t subq_bcd_to_bin(uint8_t x)
{
    return x >= 0xA0 ? x : bcd_to_bin(x);
}

/* Calculate CRC for the Q sub-channel.
 * CRC-16/GSM with length 10
 */
static inline unsigned int subq_crc(const uint8_t* subq_buf)
{
    int length = 10;
    const unsigned crc_poly = 0x1021;
    unsigned r = 0x0000;
    while (length--) {
        r ^= *subq_buf++ << 8;
        for (int i = 0; i < 8; i++)
            r = r & 0x8000 ? (r << 1) ^ crc_poly : r << 1;
    }
    return ~r & 0xFFFFU;
}

/**
 * Reads the CRC recorded in the Q sub-channel.
 */
static inline unsigned int subq_read_crc(const uint8_t *subq_buf)
{
    return (subq_buf[10] << 8) | subq_buf[11];
}

/* MMC-3 Table 38 - Formatted Q sub-channel response data */
static void subq_decode(subq_t *subq, const uint8_t *src)
{
    subq->control       = (src[0] & 0xF0) >> 4;
    subq->adr           = (src[0] & 0x0F) >> 0;
    subq->track_number  = subq_bcd_to_bin(src[1]);
    subq->index_number  = subq_bcd_to_bin(src[2]);
    subq->min           = subq_bcd_to_bin(src[3]);
    subq->sec           = subq_bcd_to_bin(src[4]);
    subq->frame         = subq_bcd_to_bin(src[5]);
    subq->amin          = subq_bcd_to_bin(src[7]);
    subq->asec          = subq_bcd_to_bin(src[8]);
    subq->aframe        = subq_bcd_to_bin(src[9]);
    subq->crc           = (src[10] << 8) | src[11];
}

static void subq_bcd_fixup(uint8_t *subq_buf)
{
    static const int fields[] = { 1, 2, 3, 4, 5, 7, 8, 9 };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint8_t x = subq_buf[fields[i]];
        subq_buf[fields[i]] = (uint8_t)(((x / 10) << 4) | (x % 10));
    }
}

/**
 * Reads Q sub-channel sector and validates its CRC, converting to BCD if needed.
 * 
 * The BCD conversion is a workround for drives that return raw binary values instead of BCD.
 *
 * Returns DRIVER_OP_SUCCESS if the sector is valid, DRIVER_OP_ERROR for CRC mismatch,
 * or another driver_return_code_t for other errors.
 */
static driver_return_code_t subq_read_valid_audio_sector(
    cyanrip_ctx *ctx,
    uint8_t *audio_subq_buf,
    const lsn_t lsn)
{
    driver_return_code_t ret = cyanrip_read_audio_subq_sector(ctx->cdio, audio_subq_buf, lsn);
    if (ret) {
        return ret;
    }

    uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;
    if (ctx->subq_needs_bcd_fixup == 1) {
        subq_bcd_fixup(subq_buf);

        return (subq_read_crc(subq_buf) == subq_crc(subq_buf) ? DRIVER_OP_SUCCESS : DRIVER_OP_ERROR);
    }

    if (subq_read_crc(subq_buf) == subq_crc(subq_buf)) {
        ctx->subq_needs_bcd_fixup = 2; /* We matched a CRC without the BCD fixup. Never apply BCD fixup to avoid false positives */
        return DRIVER_OP_SUCCESS;
    }

    if (ctx->subq_needs_bcd_fixup == 2) {
        return DRIVER_OP_ERROR;
    }

    // CRC mismatch, try to fixup BCD and see if that works
    uint8_t subq_buf_copy[SUBQ_SIZE];
    memcpy(subq_buf_copy, subq_buf, SUBQ_SIZE);
    subq_bcd_fixup(subq_buf_copy);

    if (subq_read_crc(subq_buf_copy) == subq_crc(subq_buf_copy)) {
        ctx->subq_needs_bcd_fixup = 1;

        memcpy(subq_buf, subq_buf_copy, SUBQ_SIZE);
        return DRIVER_OP_SUCCESS;
    }

    return DRIVER_OP_ERROR;
}

/**
 * Reads the Q sub-channel with retries, returning on the first successful read or the last error.
 * Increments total_failures for each failed read attempt.
 * 
 * audio_subq_buf must be at least CDIO_CD_FRAMESIZE_RAW + SUBQ_SIZE bytes.
 */
static driver_return_code_t subq_read_with_retries(
    cyanrip_ctx *ctx,
    uint8_t *audio_subq_buf,
    subq_t *subq,
    const lsn_t lsn,
    int *total_failures)
{
    driver_return_code_t ret; 
    int retry = 0;

    while (retry < SECTOR_MAX_RETRIES) {
        ret = subq_read_valid_audio_sector(ctx, audio_subq_buf, lsn);
        if (ret == DRIVER_OP_SUCCESS) {
            subq_decode(subq, audio_subq_buf + CDIO_CD_FRAMESIZE_RAW);
            break;
        }
        (*total_failures)++;
        retry++;

        // Abort if the read failure is not recoverable
        if (ret != DRIVER_OP_ERROR || *total_failures > TOTAL_FAILURE_BUDGET) {
            break;
        }
    }

    return ret;
}

/**
 * True if a failed read still leaves the search viable: a plain CRC mismatch on
 * one sector can be worked around by looking at its neighbours, as long as we
 * still have failure budget left. Anything else (a real driver/transport error)
 * ends the search.
 */
static inline int subq_read_failure_is_skippable(driver_return_code_t ret, int total_failures)
{
    return ret == DRIVER_OP_ERROR && total_failures <= TOTAL_FAILURE_BUDGET;
}

/**
 * Finds the pregap LSN of a given track by reading Q sub-channel data and validating CRCs.
 * Returns the pregap LSN if found, or CDIO_INVALID_LSN if not found or if the track is not audio.
 *
 * The pregap is the run of sectors before a track's start that already carry
 * the new track's number (index 0). The TOC doesn't say where it begins, so we
 * find that first sector by reading the Q sub-channel of individual sectors
 * between the previous track's start and this one's.
 *
 * The search keeps two bounds inside that region:
 *
 *     left_bound  - highest sector known to still be in the previous track
 *     right_bound - lowest sector known to already be in the new track
 *
 * The pregap starts at right_bound once the two bounds are adjacent. Getting
 * them there takes three steps:
 *
 *  1. Read the sector just below the track start. If it, and the one below it,
 *     still report the previous track, there is no pregap - done.
 *
 *  2. Backtrack from the track start in 2 second (150 sector) steps until a
 *     sector reports the previous track, and make that left_bound. Steps that
 *     land inside the pregap pull right_bound down on the way. Pregaps are
 *     commonly exactly 2 seconds, so the first step usually lands one sector
 *     below the answer.
 *
 *  3. Walk upwards from left_bound, moving whichever bound each sector's track
 *     number allows, restarting the walk from left_bound whenever right_bound
 *     moves, until the bounds meet.
 *
 * Two kinds of drive misbehaviour shape the rest of the code:
 *
 *  - Sectors whose Q sub-channel never passes its CRC. Rather than abandoning
 *    the search, the walk steps over them and lets a later good read rule them
 *    out by moving a bound past them; an overall failure budget keeps badly
 *    damaged media from stalling the rip. A bad sector left as the only gap
 *    between the bounds makes the pregap genuinely ambiguous, and the search
 *    gives up.
 *
 *  - Seek jitter, where a CRC-valid read describes a different sector than the
 *    one asked for. Acting on one would move a bound to a sector it doesn't
 *    belong on and produce a plausible but wrong pregap, so a bound only moves
 *    once an adjacent sector agrees with the read (this mirrors XLD's
 *    two-consecutive-reads debounce). During the upward walk that neighbour is
 *    just the previously read sector, and right_bound itself counts as an
 *    agreeing new-track sector for the sector directly below it.
 */
lsn_t cyanrip_get_track_pregap_lsn(cyanrip_ctx *ctx, const track_t track_number)
{
    // Try to use libcdio. If libcdio doesn't implement pregap finding
    // for a driver, it will return CDIO_INVALID_LSN.
    const lsn_t cdio_track_pregap_lsn = cdio_get_track_pregap_lsn(ctx->cdio, track_number);
    if (cdio_track_pregap_lsn != CDIO_INVALID_LSN)
        return cdio_track_pregap_lsn;

    /* If the track is not audio, skip pregap search */
    if (cdio_get_track_format(ctx->cdio, track_number) != TRACK_FORMAT_AUDIO) {
        cyanrip_log(ctx, 0, "Track %i is not audio, skipping pregap search\n", track_number);
        return CDIO_INVALID_LSN;
    }

    /* Detect the first track number and return 0 as the pregap lsn for the first track */
    const track_t first_track_number = cdio_get_first_track_num(ctx->cdio);
    if (track_number == first_track_number || track_number <= 1)
        return 0;

    const lsn_t track_start_lsn = cdio_get_track_lsn(ctx->cdio, track_number);
    if (track_start_lsn == CDIO_INVALID_LSN) {
        cyanrip_log(ctx, 0, "Track %i start LSN is invalid, skipping pregap search\n", track_number);
        return CDIO_INVALID_LSN;
    }

    const uint8_t prev_track_number = track_number - 1;

    // Q sub-channel pregap searching only makes sense across an audio-audio
    // track boundary: a data track's Q sub-channel doesn't carry the same
    // index/track position semantics, and running the search anyway is both
    // meaningless and wasted work (matches XLD's guard).
    if (cdio_get_track_format(ctx->cdio, prev_track_number) != TRACK_FORMAT_AUDIO)
        return CDIO_INVALID_LSN;

    const lsn_t prev_track_start_lsn = cdio_get_track_lsn(ctx->cdio, prev_track_number);
    if (prev_track_start_lsn == CDIO_INVALID_LSN) {
        cyanrip_log(ctx, 0, "Track %i start LSN is invalid, skipping pregap search\n", prev_track_number);
        return CDIO_INVALID_LSN;
    }

    /* Previous track is a single sector. No pregap */
    if (prev_track_start_lsn + 1 == track_start_lsn)
        return track_start_lsn;

    uint8_t *audio_subq_buf = av_malloc(CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ);

    lsn_t lsn;
    subq_t subq;
    driver_return_code_t ret;
    int total_failures = 0;

    // Both bounds start on sectors the TOC already vouches for: the previous
    // track's start is in the previous track, this track's start is in this
    // track. See the algorithm description in the doc comment above.
    lsn_t left_bound = prev_track_start_lsn;
    lsn_t right_bound = track_start_lsn;

    // Step 1: is there a pregap at all? The sector below the track start,
    // confirmed by the sector below that, answers it.
    lsn = track_start_lsn - 1;
    ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
    if (ret && !subq_read_failure_is_skippable(ret, total_failures))
        goto fail;

    if (!ret && subq.adr == 1 && subq.track_number == prev_track_number) {
        const lsn_t confirm_lsn = lsn - 1;
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, confirm_lsn, &total_failures);
        if (ret && !subq_read_failure_is_skippable(ret, total_failures))
            goto fail;
        if (!ret && subq.adr == 1 && subq.track_number == prev_track_number) {
            av_free(audio_subq_buf);
            return track_start_lsn;
        }
    }

    // Step 2: there is a pregap, or the reads were ambiguous. Backtrack in 2
    // second increments until a sector can be confirmed to sit below any
    // pregap; that is where the upward walk starts from. A 2 second pregap is
    // common, so this often lands right below the boundary.
    const lsn_t backtrack = 150;
    lsn = track_start_lsn - 1;
    while (1) {
        lsn = lsn - backtrack >= prev_track_start_lsn ? lsn - backtrack : prev_track_start_lsn;
        if (lsn == prev_track_start_lsn) {
            break;
        }
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
        if (ret) {
            // An unreadable landing spot tells us nothing; jump further back.
            if (subq_read_failure_is_skippable(ret, total_failures))
                continue;
            goto fail;
        }

        if (subq.adr != 1)
            continue;

        if (subq.track_number == prev_track_number) {
            // Confirm with the sector below before trusting this as the left
            // bound. A single spuriously CRC-valid read of the wrong sector
            // here would put the left bound inside the pregap, and the search
            // would then happily converge on a wrong answer. lsn is always
            // above prev_track_start_lsn here, so lsn - 1 is in range.
            ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn - 1, &total_failures);
            if (ret && !subq_read_failure_is_skippable(ret, total_failures))
                goto fail;
            if (!ret && subq.adr == 1 && subq.track_number == prev_track_number)
                break;
            continue;
        }

        // Anything other than the two tracks we're between is a read we can't
        // make sense of - keep backtracking rather than trusting it as a bound.
        if (subq.track_number != track_number)
            continue;

        // Confirm with the very next sector before trusting this jump
        // landed inside the pregap rather than on a spuriously
        // CRC-valid read of the wrong sector.
        const lsn_t confirm_lsn = lsn + 1;
        if (confirm_lsn >= track_start_lsn) {
            // track_start_lsn is known to belong to the new track already.
            right_bound = lsn;
            continue;
        }
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, confirm_lsn, &total_failures);
        if (ret && !subq_read_failure_is_skippable(ret, total_failures))
            goto fail;
        if (!ret && subq.adr == 1 && subq.track_number == track_number)
            right_bound = lsn;
    }
    left_bound = lsn;

    // Step 3: walk upwards from left_bound, moving the bounds closer together
    // on each sector that identifies itself, until they are adjacent. Sectors
    // that won't read are stepped over, in the hope that a good sector further
    // along moves a bound past them and rules them out as the pregap start.
    assert(left_bound >= prev_track_start_lsn);
    assert(right_bound <= track_start_lsn);
    assert(lsn == left_bound);
    lsn_t right_bound_candidate = CDIO_INVALID_LSN;
    while ((left_bound + 1) != right_bound) {
        lsn += 1;
        if (lsn == right_bound) {
            // Walked all the way up to right_bound without the bounds meeting,
            // so unreadable sectors are all that is left between them and
            // there is no way to tell which one starts the pregap. Give up.
            break;
        }
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
        if (ret) {
            // Leave both bounds where they are and step over this sector: a
            // later good read can still rule it out by moving a bound past it.
            if (subq_read_failure_is_skippable(ret, total_failures))
                continue;
            goto fail;
        }

        if (subq.adr != 1) {
            // Mode 2 and mode 3 Q frames carry the catalogue number or ISRC
            // instead of a position, so they can't say which track they are
            // in. One sitting directly above left_bound is taken as part of
            // the previous track, on the assumption that a pregap doesn't
            // begin on one; anywhere else it is stepped over like a sector
            // that wouldn't read.
            if (lsn - 1 == left_bound) {
                assert(lsn >= left_bound);
                left_bound = lsn;
                right_bound_candidate = CDIO_INVALID_LSN;
            }
        }
        else if (subq.track_number == prev_track_number) {
            assert(lsn >= left_bound);
            left_bound = lsn;
            right_bound_candidate = CDIO_INVALID_LSN;
        }
        else if (subq.track_number == track_number) {
            assert(lsn <= right_bound);
            // Require two consecutive sectors reporting the new track number
            // before contracting right bound: guards against a single
            // spuriously CRC-valid read of the wrong physical sector.
            if (right_bound_candidate == lsn - 1)
                right_bound = lsn - 1;
            else if (lsn + 1 == right_bound)
                // right_bound is itself an established new-track sector, so it
                // serves as the second of the two consecutive reads. Without
                // this, a pregap one sector long could never be confirmed.
                right_bound = lsn;
            else {
                right_bound_candidate = lsn;
                continue;
            }
            right_bound_candidate = CDIO_INVALID_LSN;
            // Rescan the narrowed range from the left bound.
            lsn = left_bound;
        }
    }

    if (left_bound + 1 != right_bound) {
        cyanrip_log(ctx, 0, "Warning: could not narrow down the pregap of track %i to a single "
                    "sector (unreadable sectors near the track boundary), skipping pregap detection\n",
                    track_number);
        av_free(audio_subq_buf);
        return CDIO_INVALID_LSN;
    }
    lsn = right_bound;

    av_free(audio_subq_buf);
    return lsn;

fail:
    assert(ret != DRIVER_OP_SUCCESS);
    if (total_failures > TOTAL_FAILURE_BUDGET)
        cyanrip_log(ctx, 0, "Warning: repeated subq CRC mismatches prevented finding the "
                "pregap of track %i, skipping pregap detection\n", track_number);
    else
        cyanrip_log(ctx, 0, "Warning: failed to read subq data at lsn %i (error %i) while "
                    "searching for the pregap of track %i, skipping pregap detection\n",
                    lsn, ret, track_number);

    av_free(audio_subq_buf);
    return CDIO_INVALID_LSN;
}
