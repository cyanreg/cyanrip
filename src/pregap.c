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

// CRC-16/GSM with length 10
static inline unsigned crc_subq(const uint8_t* subq_buf)
{
    int length = 10;
    const unsigned crc_poly = 0x1021;
    unsigned r = 0x0000;
    while (length--) {
        r ^= *subq_buf++ << 8;
        for (int i = 0; i < 8; i++)
            r = r & 0x8000 ? (r << 1) ^ crc_poly : r << 1;
    }
    return ~r & 0xFFFF;
}

/* MMC-3 Table 38 - Formatted Q sub-channel response data */
static void decode_subq(subq_t *subq, const uint8_t *src)
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

static void fixup_subq_to_bcd(uint8_t *subq_buf)
{
    static const int fields[] = { 1, 2, 3, 4, 5, 7, 8, 9 };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        uint8_t x = subq_buf[fields[i]];
        subq_buf[fields[i]] = (uint8_t)(((x / 10) << 4) | (x % 10));
    }
}

/**
 * Validates a Q sub-channel sector's CRC and converts to BCD if needed.
 * Workround for drives that return raw binary values instead of BCD.
 * 
 * Note: This function mutates subq_buf in place when a fixup is applied
 * so this function should be called only once per buffer.
 * 
 * Returns 1 if valid, 0 otherwise.
 */
static int verify_subq_crc(cyanrip_ctx *ctx, uint8_t *subq_buf)
{
    const unsigned crc_read = (subq_buf[10] << 8) | subq_buf[11];
    if (!crc_read)
        return 0;

    if (ctx->subq_needs_bcd_fixup) {
        fixup_subq_to_bcd(subq_buf);
        return crc_read == crc_subq(subq_buf);
    }

    if (crc_read == crc_subq(subq_buf))
        return 1;

    fixup_subq_to_bcd(subq_buf);
    if (crc_read == crc_subq(subq_buf)) {
        ctx->subq_needs_bcd_fixup = 1;
        return 1;
    }

    return 0;
}

// Returns the driver error code (if any) via the return value, and whether
// the sector ended up CRC-valid via *out_valid. Callers must use *out_valid
// rather than calling verify_subq_crc() again on the same buffer: it mutates
// the buffer in place when the BCD fixup is active, so a second call on an
// already-fixed-up buffer would re-apply the fixup and corrupt it.
static driver_return_code_t read_audio_subq_sector_with_retries(
    cyanrip_ctx *ctx,
    uint8_t *audio_subq_buf,
    const lsn_t lsn,
    const int max_retries,
    int *total_failures,
    int *out_valid)
{
    driver_return_code_t ret = cyanrip_read_audio_subq_sector(ctx->cdio, audio_subq_buf, lsn);
    uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;
    int retry = 0;
    int valid = !ret && verify_subq_crc(ctx, subq_buf);

    while (retry++ < max_retries && !valid) {
        (*total_failures)++;
        if ((ret = cyanrip_read_audio_subq_sector(ctx->cdio, audio_subq_buf, lsn))) {
            *out_valid = 0;
            return ret;
        }
        valid = verify_subq_crc(ctx, subq_buf);
        // TODO ret error handling
    }

    *out_valid = valid;
    return ret;
}

// TODO: Check that drive is actually returning Q subchannel data and not just zeroes.
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
    if (track_number == first_track_number || track_number < 1)
        return 0;

    const lsn_t track_start_lsn = cdio_get_track_lsn(ctx->cdio, track_number);
    const uint8_t prev_track_number = track_number - 1;
    const lsn_t prev_track_start_lsn = cdio_get_track_lsn(ctx->cdio, prev_track_number);

    // Q sub-channel pregap searching only makes sense across an audio-audio
    // track boundary: a data track's Q sub-channel doesn't carry the same
    // index/track position semantics, and running the search anyway is both
    // meaningless and wasted work (matches XLD's guard).
    if (cdio_get_track_format(ctx->cdio, prev_track_number) != TRACK_FORMAT_AUDIO)
        return CDIO_INVALID_LSN;

    // Handle single sector previous track.
    if (prev_track_start_lsn + 1 == track_start_lsn)
        return track_start_lsn;

    uint8_t *audio_subq_buf = av_malloc(CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ);
    uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;

    lsn_t lsn;
    subq_t subq;
    // UltraFuzzy: Based on brief informal testing, successful subchannel Q read
    // retries become rare after ~5-10 attempts but I've seen a correct read
    // first occur as late as 180 attempts. The large harder_retry_max value
    // will only be used for bad sectors that cannot be ruled out as possibly
    // containing the start of a pregap. This should be rare and when it does
    // happen these sectors must be read for pregap finding to succeed.
    int retry_max = 5;
    const int harder_retry_max = 200;
    driver_return_code_t ret;

    // Overall budget on how many failed (CRC-invalid) reads we'll tolerate
    // across the whole search before giving up entirely, so that severely
    // damaged media near a track boundary can't stall ripping indefinitely
    // (XLD has an equivalent global cap).
    int total_failures = 0;
    const int total_failure_budget = 1000;

    // The main idea of this algorithm is to track a left bound, representing
    // our current latest known sector that belongs to the previous track, and a
    // right bound, representing our current earliest known sector that belongs
    // to the pregap. We traverse between our known boundaries contracting
    // them when possible until they converge or until CRC mismatches make that
    // impossible. When encountering bad sectors with repeated CRC mismatches we
    // sometimes attempt to skip over them and find a good sector that we can
    // use to contract our bounds and rule out the bad sectors from containing
    // the start of the pregap.
    //
    // A single CRC-valid Q sub-channel read is not enough on its own to trust a
    // track-number transition: drive seek/read jitter near a track boundary can
    // occasionally return a genuinely CRC-valid read for the wrong physical
    // sector. Wherever this algorithm is about to trust a transition, it first
    // confirms it with an adjacent read (mirroring XLD's two-consecutive-reads
    // debounce against the same quirk) before contracting a bound.
    lsn_t left_bound = prev_track_start_lsn;
    lsn_t right_bound = track_start_lsn;

    // Check the sector immediately before track start, confirmed by the
    // sector before that, to see if there is no pregap at all.
    int subq_valid;
    lsn = track_start_lsn - 1;
    ret = read_audio_subq_sector_with_retries(ctx, audio_subq_buf, lsn, retry_max, &total_failures, &subq_valid);
    if (ret)
        goto fail;
    if (subq_valid) {
        decode_subq(&subq, subq_buf);
        if (subq.adr == 1 && subq.track_number == prev_track_number) {
            const lsn_t confirm_lsn = lsn - 1;
            ret = read_audio_subq_sector_with_retries(ctx, audio_subq_buf, confirm_lsn, retry_max, &total_failures, &subq_valid);
            if (ret)
                goto fail;
            if (subq_valid) {
                decode_subq(&subq, subq_buf);
                if (subq.adr == 1 && subq.track_number == prev_track_number) {
                    av_free(audio_subq_buf);
                    return track_start_lsn;
                }
            }
        }
    }

    // There is a pregap or the result was ambiguous. Backtrack in 2
    // second increments until we find a position that can be confirmed to be
    // before any pregap. A 2 second pregap is common so this will often
    // backtrack right to the pregap boundary.
    const lsn_t backtrack = 150;
    lsn = track_start_lsn - 1;
    while (1) {
        lsn = lsn - backtrack >= prev_track_start_lsn ? lsn - backtrack : prev_track_start_lsn;
        if (lsn == prev_track_start_lsn) {
            break;
        }
        ret = read_audio_subq_sector_with_retries(ctx, audio_subq_buf, lsn, retry_max, &total_failures, &subq_valid);
        if (ret)
            goto fail;
        if (total_failures > total_failure_budget)
            goto giveup;
        if (!subq_valid) {
            continue;
        }
        decode_subq(&subq, subq_buf);
        if (subq.adr != 1) {
            continue;
        }
        else if (subq.track_number == track_number) {
            // Confirm with the very next sector before trusting this jump
            // landed inside the pregap rather than on a spuriously
            // CRC-valid read of the wrong sector.
            const lsn_t confirm_lsn = lsn + 1;
            if (confirm_lsn >= track_start_lsn) {
                right_bound = lsn;
                continue;
            }
            ret = read_audio_subq_sector_with_retries(ctx, audio_subq_buf, confirm_lsn, retry_max, &total_failures, &subq_valid);
            if (ret)
                goto fail;
            if (total_failures > total_failure_budget)
                goto giveup;
            if (subq_valid) {
                decode_subq(&subq, subq_buf);
                if (subq.adr == 1 && subq.track_number == track_number)
                    right_bound = lsn;
            }
        }
        else {
            assert(subq.track_number == prev_track_number);
            break;
        }
    }
    left_bound = lsn;

    // Loop over sectors from left bound to right bound attempting to contract
    // bounds until they meet. Skip over sectors with repeated bad CRCs and
    // attempt to find a good sector that allows us to contract the bounds and
    // rule out those bad sectors as the pregap start.
    assert(left_bound >= prev_track_start_lsn);
    assert(right_bound <= track_start_lsn);
    assert(lsn == left_bound);
    lsn_t right_bound_candidate = CDIO_INVALID_LSN;
    while ((left_bound + 1) != right_bound) {
        lsn += 1;
        if (lsn == right_bound) {
            // Skipping over sectors with bad CRCs failed.
            // If we've already been here, give up.
            if (retry_max == harder_retry_max)
                break;
            // Getting here means we skipped over bad sectors that it turns must
            // be read in order to decide where the pregap starts. TRY HARDER.
            retry_max = harder_retry_max;
            lsn = left_bound;
            right_bound_candidate = CDIO_INVALID_LSN;
            continue;
        }
        ret = read_audio_subq_sector_with_retries(ctx, audio_subq_buf, lsn, retry_max, &total_failures, &subq_valid);
        if (ret)
            goto fail;
        if (total_failures > total_failure_budget)
            goto giveup;
        if (!subq_valid) {
            // Attempt to skip over sectors with bad CRCs.
            continue;
        }
        decode_subq(&subq, subq_buf);
        if (subq.adr != 1) {
            // If a mode 2 or mode 3 sector immediately follows left bound,
            // consider it part of previous track and contract left bound.
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
            assert(subq.index_number == 0);
            // Require two consecutive sectors reporting the new track number
            // before contracting right bound: guards against a single
            // spuriously CRC-valid read of the wrong physical sector.
            if (right_bound_candidate == lsn - 1) {
                right_bound = right_bound_candidate;
                right_bound_candidate = CDIO_INVALID_LSN;
                // Restart loop.
                lsn = left_bound;
            } else {
                right_bound_candidate = lsn;
            }
        }
    }
    if (left_bound + 1 == right_bound)
        lsn = right_bound;
    else
        goto giveup;
    

    av_free(audio_subq_buf);
    return lsn;

giveup:
    cyanrip_log(ctx, 0, "Warning: repeated subq CRC mismatches prevented finding the "
                "pregap of track %i, skipping pregap detection\n", track_number);
    av_free(audio_subq_buf);
    return CDIO_INVALID_LSN;

fail:
    cyanrip_log(ctx, 0, "Warning: failed to read subq data at lsn %i (error %i) while "
                "searching for the pregap of track %i, skipping pregap detection\n",
                lsn, ret, track_number);
    av_free(audio_subq_buf);
    return CDIO_INVALID_LSN;
}
