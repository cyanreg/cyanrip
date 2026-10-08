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

/* Max sectors between the bounds for the damaged-frame pass */
#define SUBQ_DAMAGED_MAX_GAP 8

static inline int subq_read_failure_is_skippable(driver_return_code_t ret, int total_failures)
{
    return ret == DRIVER_OP_ERROR && total_failures <= TOTAL_FAILURE_BUDGET;
}

/*
 * Narrows two bounds between the previous track's start and this one's:
 * left_bound is the last sector known to be in the previous track,
 * right_bound the first known to be in this one.
 */
lsn_t cyanrip_get_track_pregap_lsn(cyanrip_ctx *ctx, const track_t track_number, cyanrip_pregap_info *info)
{
    cyanrip_pregap_info unused_info;
    if (!info)
        info = &unused_info;
    memset(info, 0, sizeof(*info));
    info->failed_lsn = CDIO_INVALID_LSN;

    /* Not implemented by most libcdio drivers */
    const lsn_t cdio_track_pregap_lsn = cdio_get_track_pregap_lsn(ctx->cdio, track_number);
    if (cdio_track_pregap_lsn != CDIO_INVALID_LSN)
        return cdio_track_pregap_lsn;

    if (cdio_get_track_format(ctx->cdio, track_number) != TRACK_FORMAT_AUDIO) {
        cyanrip_log(ctx, 0, "Track %i is not audio, skipping pregap search\n", track_number);
        return CDIO_INVALID_LSN;
    }

    const lsn_t track_start_lsn = cdio_get_track_lsn(ctx->cdio, track_number);
    if (track_start_lsn == CDIO_INVALID_LSN) {
        cyanrip_log(ctx, 0, "Track %i start LSN is invalid, skipping pregap search\n", track_number);
        return CDIO_INVALID_LSN;
    }

    /* First track's pregap is the start of the disc */
    const track_t first_track_number = cdio_get_first_track_num(ctx->cdio);
    if (track_number == first_track_number || track_number <= 1)
        return ctx->start_lsn;

    const track_t prev_track_number = track_number - 1;

    if (cdio_get_track_format(ctx->cdio, prev_track_number) != TRACK_FORMAT_AUDIO)
        return CDIO_INVALID_LSN;

    const lsn_t prev_track_start_lsn = cdio_get_track_lsn(ctx->cdio, prev_track_number);
    if (prev_track_start_lsn == CDIO_INVALID_LSN) {
        cyanrip_log(ctx, 0, "Track %i start LSN is invalid, skipping pregap search\n", prev_track_number);
        return CDIO_INVALID_LSN;
    }

    if (prev_track_start_lsn + 1 == track_start_lsn)
        return CDIO_INVALID_LSN;

    uint8_t *audio_subq_buf = av_malloc(CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW);

    subq_probe_read_mode(ctx, audio_subq_buf, prev_track_start_lsn, track_start_lsn);

    lsn_t lsn;
    subq_t subq;
    driver_return_code_t ret;
    int total_failures = 0;

    lsn_t left_bound = prev_track_start_lsn;
    lsn_t right_bound = track_start_lsn;
    int right_bound_is_pregap = 0; /* index 0 */

    /* Absolute time of the Q frames at the bounds, if any */
    lsn_t left_bound_abs_lsn = CDIO_INVALID_LSN;
    lsn_t right_bound_abs_lsn = CDIO_INVALID_LSN;

    /* No pregap if the two sectors below the track start are the previous track */
    lsn = track_start_lsn - 1;
    ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
    if (ret && !subq_read_failure_is_skippable(ret, total_failures))
        goto fail;

    if (!ret && subq.adr == 1 && subq.track_number == prev_track_number) {
        const lsn_t confirm_lsn = lsn - 1;
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, confirm_lsn, &total_failures);
        if (ret && !subq_read_failure_is_skippable(ret, total_failures)) {
            lsn = confirm_lsn;
            goto fail;
        }
        if (!ret && subq.adr == 1 && subq.track_number == prev_track_number) {
            av_free(audio_subq_buf);
            return CDIO_INVALID_LSN;
        }
    }

    /* Backtrack 2 seconds at a time until a sector below the pregap is confirmed */
    const lsn_t backtrack = 150;
    lsn = track_start_lsn - 1;
    while (1) {
        lsn = lsn - backtrack >= prev_track_start_lsn ? lsn - backtrack : prev_track_start_lsn;
        if (lsn == prev_track_start_lsn)
            break;
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
        if (ret) {
            if (subq_read_failure_is_skippable(ret, total_failures))
                continue;
            goto fail;
        }

        if (subq.adr != 1)
            continue;

        if (subq.track_number == prev_track_number) {
            left_bound_abs_lsn = subq_abs_lsn(&subq);

            /* Confirm with the sector below, a wrong left bound can't be recovered from */
            ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn - 1, &total_failures);
            if (ret && !subq_read_failure_is_skippable(ret, total_failures)) {
                lsn = lsn - 1;
                goto fail;
            }
            if (!ret && subq.adr == 1 && subq.track_number == prev_track_number)
                break;
            continue;
        }

        if (subq.track_number != track_number)
            continue;

        /* Confirm with the sector above */
        const int is_pregap = subq.index_number == 0;
        const lsn_t abs_lsn = subq_abs_lsn(&subq);
        const lsn_t confirm_lsn = lsn + 1;
        if (confirm_lsn >= track_start_lsn) {
            right_bound = lsn;
            right_bound_is_pregap = is_pregap;
            right_bound_abs_lsn = abs_lsn;
            continue;
        }
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, confirm_lsn, &total_failures);
        if (ret && !subq_read_failure_is_skippable(ret, total_failures)) {
            lsn = confirm_lsn;
            goto fail;
        }
        if (!ret && subq.adr == 1 && subq.track_number == track_number) {
            right_bound = lsn;
            right_bound_is_pregap = is_pregap;
            right_bound_abs_lsn = abs_lsn;
        }
    }
    left_bound = lsn;
    if (left_bound == prev_track_start_lsn)
        left_bound_abs_lsn = CDIO_INVALID_LSN;

    /* Walk up from left_bound until the bounds meet, stepping over unreadable
     * sectors. right_bound only moves once a second new-track sector above
     * the candidate agrees. */
    assert(left_bound >= prev_track_start_lsn);
    assert(right_bound <= track_start_lsn);
    assert(lsn == left_bound);
    lsn_t right_bound_candidate = CDIO_INVALID_LSN;
    int right_bound_candidate_is_pregap = 0;
    lsn_t right_bound_candidate_abs_lsn = CDIO_INVALID_LSN;
    while ((left_bound + 1) != right_bound) {
        int confirmed = 0;

        lsn += 1;
        if (lsn == right_bound) {
            /* Only unreadable sectors left */
            if (right_bound_candidate == CDIO_INVALID_LSN)
                break;

            /* right_bound confirms the candidate */
            confirmed = 1;
        } else {
            ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
            if (ret) {
                if (subq_read_failure_is_skippable(ret, total_failures))
                    continue;
                goto fail;
            }

            if (subq.adr != 1) {
                /* No position in mode 2/3 frames, assume a pregap doesn't start on one */
                if (lsn - 1 == left_bound) {
                    assert(right_bound_candidate == CDIO_INVALID_LSN);
                    left_bound = lsn;
                    left_bound_abs_lsn = CDIO_INVALID_LSN;
                }
            } else if (subq.track_number == prev_track_number) {
                assert(lsn >= left_bound);
                left_bound = lsn;
                left_bound_abs_lsn = subq_abs_lsn(&subq);
                right_bound_candidate = CDIO_INVALID_LSN;
            } else if (subq.track_number == track_number) {
                assert(lsn <= right_bound);
                if (right_bound_candidate == CDIO_INVALID_LSN) {
                    right_bound_candidate = lsn;
                    right_bound_candidate_is_pregap = subq.index_number == 0;
                    right_bound_candidate_abs_lsn = subq_abs_lsn(&subq);
                } else {
                    confirmed = 1;
                }
            }
        }

        if (confirmed) {
            right_bound = right_bound_candidate;
            right_bound_is_pregap = right_bound_candidate_is_pregap;
            right_bound_abs_lsn = right_bound_candidate_abs_lsn;
            right_bound_candidate = CDIO_INVALID_LSN;
            lsn = left_bound;
        }
    }

    /* Close the remaining gap with damaged frames from both sides */
    int nb_damaged_used = 0, nb_repaired = 0, repaired;
    if (left_bound + 1 != right_bound && right_bound - left_bound - 1 <= SUBQ_DAMAGED_MAX_GAP) {
        while (left_bound + 1 != right_bound) {
            lsn = left_bound + 1;
            if (!subq_read_damaged(ctx, audio_subq_buf, &subq, lsn, prev_track_number, track_number, &repaired))
                break;
            if (subq.track_number != prev_track_number)
                break;
            left_bound = lsn;
            left_bound_abs_lsn = lsn;
            nb_damaged_used++;
            nb_repaired += repaired;
        }
        while (left_bound + 1 != right_bound) {
            lsn = right_bound - 1;
            if (!subq_read_damaged(ctx, audio_subq_buf, &subq, lsn, prev_track_number, track_number, &repaired))
                break;
            const int is_pregap = subq.index_number == 0;
            /* Index can't go from 1 back to 0 */
            if (subq.track_number != track_number || (right_bound_is_pregap && !is_pregap))
                break;
            right_bound = lsn;
            right_bound_is_pregap = is_pregap;
            right_bound_abs_lsn = lsn;
            nb_damaged_used++;
            nb_repaired += repaired;
        }
    }

    if (left_bound + 1 != right_bound) {
        cyanrip_log(ctx, 0, "Warning: could not narrow down the pregap of track %i to a single "
                    "sector (unreadable sectors near the track boundary), skipping pregap detection\n",
                    track_number);
        info->result = CYANRIP_PREGAP_SEARCH_UNREADABLE;
        av_free(audio_subq_buf);
        return CDIO_INVALID_LSN;
    }

    info->result = CYANRIP_PREGAP_SEARCH_DONE;
    info->damaged_frames = nb_damaged_used;
    info->repaired_frames = nb_repaired;

    /* Index 1 at right_bound means the Q sub-channel runs ahead of the TOC */
    lsn = right_bound_is_pregap ? right_bound : CDIO_INVALID_LSN;

    /* Drives may return the Q frame of a nearby sector, use its absolute time
     * if the frames on both bounds agree */
    if (lsn != CDIO_INVALID_LSN &&
        left_bound_abs_lsn != CDIO_INVALID_LSN &&
        left_bound_abs_lsn + 1 == right_bound_abs_lsn &&
        right_bound_abs_lsn > prev_track_start_lsn &&
        right_bound_abs_lsn < track_start_lsn) {
        info->q_skew = right_bound_abs_lsn - right_bound;
        lsn = right_bound_abs_lsn;
    }

    av_free(audio_subq_buf);
    return lsn;

fail:
    assert(ret != DRIVER_OP_SUCCESS);
    if (total_failures > TOTAL_FAILURE_BUDGET) {
        cyanrip_log(ctx, 0, "Warning: repeated subq CRC mismatches prevented finding the "
                "pregap of track %i, skipping pregap detection\n", track_number);
        info->result = CYANRIP_PREGAP_SEARCH_CRC_BUDGET;
    } else {
        cyanrip_log(ctx, 0, "Warning: failed to read subq data at lsn %i (error %i) while "
                    "searching for the pregap of track %i, skipping pregap detection\n",
                    lsn, ret, track_number);
        info->result = CYANRIP_PREGAP_SEARCH_READ_ERROR;
        info->failed_lsn = lsn;
        info->failed_error = ret;
    }

    av_free(audio_subq_buf);
    return CDIO_INVALID_LSN;
}
