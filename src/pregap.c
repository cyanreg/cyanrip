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

/* How many sectors between the bounds the damaged-frame pass will look at. */
#define SUBQ_DAMAGED_MAX_GAP 8

/* Whether we can skip a subq read failure and continue searching for the pregap. */
static inline int subq_read_failure_is_skippable(driver_return_code_t ret, int total_failures)
{
    return ret == DRIVER_OP_ERROR && total_failures <= TOTAL_FAILURE_BUDGET;
}

/**
 * Finds the pregap LSN of the track by reading Q sub-channel data and validating CRCs.
 * Returns the pregap LSN if found, or CDIO_INVALID_LSN if not found or if the track is not audio.
 *
 * The LSN is found by a search that narrows two bounds between the previous track's start and this one's:
 *     left_bound  - highest sector known to still be in the previous track
 *     right_bound - lowest sector known to already be in the new track
 *
 * Only sectors of the new track carrying index 0 are pregap. The Q sub-channel
 * often runs a few sectors ahead of the TOC, so sectors just below the track
 * start may already report index 1: those belong to the track itself and are
 * never reported as a pregap.
 *
 * Drives hand back the Q of a sector a few sectors away from the one asked
 * for. The search works in terms of the sectors it asks for, which puts the
 * boundary off by as much. Each Q frame carries its own absolute time though,
 * so once the boundary is found, the frame on it says where it really is.
 */
lsn_t cyanrip_get_track_pregap_lsn(cyanrip_ctx *ctx, const track_t track_number, cyanrip_pregap_info *info)
{
    cyanrip_pregap_info unused_info;
    if (!info)
        info = &unused_info;
    memset(info, 0, sizeof(*info));
    info->failed_lsn = CDIO_INVALID_LSN;

    /* Try to use libcdio. If libcdio doesn't implement pregap finding
       for a driver, it will return CDIO_INVALID_LSN. */
    const lsn_t cdio_track_pregap_lsn = cdio_get_track_pregap_lsn(ctx->cdio, track_number);
    if (cdio_track_pregap_lsn != CDIO_INVALID_LSN)
        return cdio_track_pregap_lsn;

    /* If the track is not audio, skip pregap search */
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

    const uint8_t prev_track_number = track_number - 1;

    /* Previous track is not audio */
    if (cdio_get_track_format(ctx->cdio, prev_track_number) != TRACK_FORMAT_AUDIO)
        return CDIO_INVALID_LSN;

    const lsn_t prev_track_start_lsn = cdio_get_track_lsn(ctx->cdio, prev_track_number);
    if (prev_track_start_lsn == CDIO_INVALID_LSN) {
        cyanrip_log(ctx, 0, "Track %i start LSN is invalid, skipping pregap search\n", prev_track_number);
        return CDIO_INVALID_LSN;
    }

    /* Previous track is a single sector. No pregap */
    if (prev_track_start_lsn + 1 == track_start_lsn)
        return CDIO_INVALID_LSN;

    uint8_t *audio_subq_buf = av_malloc(CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW);

    /* Both tracks are audio by now, so anything from the previous track's
     * start up to this track's start will do for the probe. */
    subq_probe_read_mode(ctx, audio_subq_buf, prev_track_start_lsn, track_start_lsn);

    lsn_t lsn;
    subq_t subq;
    driver_return_code_t ret;
    int total_failures = 0;

    /* Both bounds start on sectors the TOC already vouches for: the previous
     * track's start is in the previous track, this track's start is in this
     * track. See the algorithm description in the doc comment above. */
    lsn_t left_bound = prev_track_start_lsn;
    lsn_t right_bound = track_start_lsn;

    /* Whether the sector at right_bound has index 0, i.e. is pregap rather
     * than the track itself. The track start is not. */
    int right_bound_is_pregap = 0;

    /* The sectors the Q frames read at the bounds say they are from, or
     * CDIO_INVALID_LSN when a bound doesn't rest on a mode 1 Q frame. */
    lsn_t left_bound_abs_lsn = CDIO_INVALID_LSN;
    lsn_t right_bound_abs_lsn = CDIO_INVALID_LSN;

    /* Step 1: is there a pregap at all? The sector below the track start,
     * confirmed by the sector below that, answers it. */
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
            return CDIO_INVALID_LSN;
        }
    }

    /* Step 2: there is a pregap, or the reads were ambiguous. Backtrack in 2
     * second increments until a sector can be confirmed to sit below any
     * pregap; that is where the upward walk starts from. A 2 second pregap is
     * common, so this often lands right below the boundary. */
    const lsn_t backtrack = 150;
    lsn = track_start_lsn - 1;
    while (1) {
        lsn = lsn - backtrack >= prev_track_start_lsn ? lsn - backtrack : prev_track_start_lsn;
        if (lsn == prev_track_start_lsn) {
            break;
        }
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
        if (ret) {
            /* An unreadable landing spot tells us nothing; jump further back. */
            if (subq_read_failure_is_skippable(ret, total_failures))
                continue;
            goto fail;
        }

        if (subq.adr != 1)
            continue;

        if (subq.track_number == prev_track_number) {
            left_bound_abs_lsn = subq_abs_lsn(&subq);

            /* Confirm with the sector below before trusting this as the left
             * bound. A single spuriously CRC-valid read of the wrong sector
             * here would put the left bound inside the pregap, and the search
             * would then happily converge on a wrong answer. lsn is always
             * above prev_track_start_lsn here, so lsn - 1 is in range. */
            ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn - 1, &total_failures);
            if (ret && !subq_read_failure_is_skippable(ret, total_failures))
                goto fail;
            if (!ret && subq.adr == 1 && subq.track_number == prev_track_number)
                break;
            continue;
        }

        /* Anything other than the two tracks we're between is a read we can't
         * make sense of - keep backtracking rather than trusting it as a bound. */
        if (subq.track_number != track_number)
            continue;

        /* Confirm with the very next sector before trusting this jump
         * landed inside the new track rather than on a spuriously
         * CRC-valid read of the wrong sector. */
        const int is_pregap = subq.index_number == 0;
        const lsn_t abs_lsn = subq_abs_lsn(&subq);
        const lsn_t confirm_lsn = lsn + 1;
        if (confirm_lsn >= track_start_lsn) {
            /* track_start_lsn is known to belong to the new track already. */
            right_bound = lsn;
            right_bound_is_pregap = is_pregap;
            right_bound_abs_lsn = abs_lsn;
            continue;
        }
        ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, confirm_lsn, &total_failures);
        if (ret && !subq_read_failure_is_skippable(ret, total_failures))
            goto fail;
        if (!ret && subq.adr == 1 && subq.track_number == track_number) {
            right_bound = lsn;
            right_bound_is_pregap = is_pregap;
            right_bound_abs_lsn = abs_lsn;
        }
    }
    left_bound = lsn;
    if (left_bound == prev_track_start_lsn)
        left_bound_abs_lsn = CDIO_INVALID_LSN; /* rests on the TOC, not on a Q frame */

    /* Step 3: walk upwards from left_bound, moving the bounds closer together
     * on each sector that identifies itself, until they are adjacent. Sectors
     * that won't read are stepped over, in the hope that a good sector further
     * along moves a bound past them and rules them out as the pregap start.
     *
     * right_bound only contracts onto a sector reporting the new track once a
     * second new-track sector above it agrees: guards against a single
     * spuriously CRC-valid read of the wrong physical sector. The two don't
     * have to be adjacent. Sectors in between that say nothing about the track
     * (unreadable, or mode 2/3 Q frames) leave the candidate standing; only a
     * sector reporting the previous track throws it out. */
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
            /* Walked all the way up to right_bound without the bounds meeting
             * and with no candidate: unreadable sectors are all that is left
             * between them and there is no way to tell which one starts the
             * pregap. Give up. */
            if (right_bound_candidate == CDIO_INVALID_LSN)
                break;

            /* right_bound is itself an established new-track sector, so it
             * serves as the second read confirming the candidate. Without
             * this, a pregap one sector long could never be confirmed. */
            confirmed = 1;
        } else {
            ret = subq_read_with_retries(ctx, audio_subq_buf, &subq, lsn, &total_failures);
            if (ret) {
                /* Leave both bounds where they are and step over this sector: a
                 * later good read can still rule it out by moving a bound past it. */
                if (subq_read_failure_is_skippable(ret, total_failures))
                    continue;
                goto fail;
            }

            if (subq.adr != 1) {
                /* Mode 2 and mode 3 Q frames carry the catalogue number or ISRC
                 * instead of a position, so they can't say which track they are
                 * in. One sitting directly above left_bound is taken as part of
                 * the previous track, on the assumption that a pregap doesn't
                 * begin on one; anywhere else it is stepped over like a sector
                 * that wouldn't read. */
                if (lsn - 1 == left_bound) {
                    assert(right_bound_candidate == CDIO_INVALID_LSN);
                    left_bound = lsn;
                    left_bound_abs_lsn = CDIO_INVALID_LSN;
                }
            }
            else if (subq.track_number == prev_track_number) {
                assert(lsn >= left_bound);
                left_bound = lsn;
                left_bound_abs_lsn = subq_abs_lsn(&subq);
                right_bound_candidate = CDIO_INVALID_LSN;
            }
            else if (subq.track_number == track_number) {
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
            /* Rescan the narrowed range from the left bound. */
            lsn = left_bound;
        }
    }

    /* Step 4: only sectors that wouldn't read are left between the bounds.
     * Their Q frames failed the CRC, but if their payload survived they can
     * still close the gap: a sector reporting the previous track directly
     * above left_bound extends it, a sector reporting the new track directly
     * below right_bound extends that. Neither may leapfrog the other. */
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
            /* The index can't drop back to 0 once the track proper has begun */
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

    /* The new track begins at right_bound, but unless that sector has index 0
     * it is the track itself showing up ahead of the TOC, not a pregap. */
    lsn = right_bound_is_pregap ? right_bound : CDIO_INVALID_LSN;

    /* right_bound is the sector that was asked for; the Q frame that came back
     * says which sector it is really from. Go by that, as long as the frames
     * on both bounds agree they are neighbours, which shows the drive was off
     * by the same amount for both, and the result still makes for a pregap. */
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
