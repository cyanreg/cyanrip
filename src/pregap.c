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

#include <stdlib.h>
#include <stdint.h>

// remove after testing
#include <inttypes.h>
#ifdef N_DEBUG
#undef N_DEBUG
#endif
#include <assert.h>

#include <cdio/cdio.h>
#include <cdio/mmc_ll_cmds.h>
// #include <cdio/iso9660.h>

// Reading Q subchannel using the READ CD command is an MMC-2 feature. Ancient
// drives don't support it and will return zeroes.
static driver_return_code_t read_audio_subq_sector(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn)
{
    return cyanrip_read_audio_subq_sectors(p_cdio, audio_subq_buf, lsn, 1);
}

// #pragma pack(push, 1)
// typedef struct subq_encoded_t {
//     uint8_t  control:4;
//     uint8_t  adr    :4;
//     uint8_t  track_number;
//     uint8_t  index_number;
//     uint8_t  min;
//     uint8_t  sec;
//     uint8_t  frame;
//     uint8_t  zero;
//     uint8_t  amin;
//     uint8_t  asec;
//     uint8_t  aframe;
//     uint16_t crc;
//     uint8_t  reserved[4];
// } subq_encoded_t;
// #pragma pack(pop)

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

static inline uint8_t bcd_to_bin(uint8_t x){
    return 10*((x & 0xF0) >> 4) + (x & 0x0F);
}

/* MMC-3 4.1.3.2.1. Q sub-channel Mode-1: "Bytes in the Q sub-channel that
 * contains bcd contents may also contain illegal BCD values. Then values start
 * with 0A0h and continue to 0FFh. No conversion of these to hex for
 * transmission to/from the initiator is performed."
 */
static inline uint8_t subq_bcd_to_bin(uint8_t x){
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

// MMC-3 Table 38 - Formatted Q sub-channel response data
static void decode_subq(subq_t *subq, const uint8_t *src) {
    subq->control       = (src[0] & 0xF0) >> 4;
    subq->adr           = (src[0] & 0x0F) >> 0;
    // TODO Unclear if these will always be BCD.  From MMC-3, the answer is yes.
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


static driver_return_code_t read_audio_subq_sector_with_retries(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn,
    const int retry_max)
{
    driver_return_code_t ret = read_audio_subq_sector(p_cdio, audio_subq_buf, lsn);
    const uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;
    unsigned crc_read = (subq_buf[10] << 8) | subq_buf[11];
    unsigned crc_comp = crc_subq(subq_buf);
    int retry = 0;
    while (retry++ < retry_max && crc_read != crc_comp) {
        // TODO Is a cache defeat here ever necessary? Testing on macOS with an
        // ASUS SDRW-08U7M-U, it didn't have an effect.
        // overflow_device_read_cache(p_cdio, lsn);
        if ((ret = read_audio_subq_sector(p_cdio, audio_subq_buf, lsn)))
            return ret;
        // TODO ret error handling
        crc_read = (subq_buf[10] << 8) | subq_buf[11];
        crc_comp = crc_subq(subq_buf);
    }
    return ret;
}


// TODO: Check that drive is actually returning Q subchannel data and not just zeroes.
lsn_t cyanrip_get_track_pregap_lsn(CdIo_t *p_cdio, const track_t track_number) {
    // Try to use libcdio. If libcdio doesn't implement pregap finding
    // for a driver, it will return CDIO_INVALID_LSN.
    const lsn_t cdio_track_pregap_lsn = cdio_get_track_pregap_lsn(p_cdio, track_number);
    if (cdio_track_pregap_lsn != CDIO_INVALID_LSN)
        return cdio_track_pregap_lsn;

    // First track pregap is lsn = 0, lba = CDIO_PREGAP_SECTORS.
    // TODO Under what circumstances does libcdio give a first track not equal
    // to 1? Does this ever happen for a rippable CD?
    const track_t first_track_number = cdio_get_first_track_num(p_cdio);
    if (track_number == first_track_number)
        return 0;

    const lsn_t track_start_lsn = cdio_get_track_lsn(p_cdio, track_number);
    // TODO Is (track_number - 1) always safe? e.g. non-continuous track numbers?
    const uint8_t prev_track_number = track_number - 1;
    const lsn_t prev_track_start_lsn = cdio_get_track_lsn(p_cdio, prev_track_number);

    // Handle single sector previous track.
    if (prev_track_start_lsn + 1 == track_start_lsn)
        return track_start_lsn;

    uint8_t *audio_subq_buf = malloc(CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ);
    const uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;

    lsn_t lsn;
    subq_t subq;
    unsigned crc_comp;
    // UltraFuzzy: Based on brief informal testing, successful subchannel Q read
    // retries become rare after ~5-10 attempts but I've seen a correct read
    // first occur as late as 180 attempts. The large harder_retry_max value
    // will only be used for bad sectors that cannot be ruled out as possibly
    // containing the start of a pregap. This should be rare and when it does
    // happen these sectors must be read for pregap finding to succeed.
    int retry_max = 5;
    const int harder_retry_max = 200;
    driver_return_code_t ret;

    // The main idea of this algorithm is to track a left bound, representing
    // our current latest known sector that belongs to the previous track, and a
    // right bound, representing our current earliest known sector that belongs
    // to the pregap. We traverse between our known boundaries contracting
    // them when possible until they converge or until CRC mismatches make that
    // impossible. When encountering bad sectors with repeated CRC mismatches we
    // sometimes attempt to skip over them and find a good sector that we can
    // use to contract our bounds and rule out the bad sectors from containing
    // the start of the pregap.
    lsn_t left_bound = prev_track_start_lsn;
    lsn_t right_bound = track_start_lsn;

    // Check one sector before track start to see if there is any pregap.
    lsn = track_start_lsn - 1;
    ret = read_audio_subq_sector_with_retries(p_cdio, audio_subq_buf, lsn, retry_max);
    assert(!ret);
    decode_subq(&subq, subq_buf);
    crc_comp = crc_subq(subq_buf);
    if (subq.crc == crc_comp && subq.adr == 1 && subq.track_number == prev_track_number)
        return track_start_lsn;

    if (subq.crc == crc_comp && subq.adr == 1 && subq.track_number == track_number)
        right_bound = lsn;

    // There is a pregap or the result was ambiguous. Backtrack in 2
    // second increments until we find a position that can be confirmed to be
    // before any pregap. A 2 second pregap is common so this will often
    // backtrack right to the pregap boundary.
    const lsn_t backtrack = 150;
    for (;;) {
        lsn = lsn - backtrack >= prev_track_start_lsn ? lsn - backtrack : prev_track_start_lsn;
        if (lsn == prev_track_start_lsn) {
            break;
        }
        ret = read_audio_subq_sector_with_retries(p_cdio, audio_subq_buf, lsn, retry_max);
        assert(!ret);
        decode_subq(&subq, subq_buf);
        crc_comp = crc_subq(subq_buf);
        if (subq.crc != crc_comp || subq.adr != 1) {
            continue;
        }
        else if (subq.track_number == track_number) {
            right_bound = lsn;
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
            continue;
        }
        ret = read_audio_subq_sector_with_retries(p_cdio, audio_subq_buf, lsn, retry_max);
        assert(!ret);
        decode_subq(&subq, subq_buf);
        crc_comp = crc_subq(subq_buf);
        if (subq.crc != crc_comp) {
            // Attempt to skip over sectors with bad CRCs.
            continue;
        }
        else if (subq.adr != 1) {
            // If a mode 2 or mode 3 sector immediately follows left bound,
            // consider it part of previous track and contract left bound.
            if (lsn - 1 == left_bound) {
                assert(lsn >= left_bound);
                left_bound = lsn;
            }
        }
        else if (subq.track_number == prev_track_number) {
            assert(lsn >= left_bound);
            left_bound = lsn;
        }
        else if (subq.track_number == track_number) {
            assert(lsn <= right_bound);
            right_bound = lsn;
            // Restart loop.
            lsn = left_bound;
        }
    }
    // TODO Log failure to find pregap due to CRC mismatches.
    lsn = (left_bound + 1 == right_bound) ? right_bound : DRIVER_OP_ERROR;

    free(audio_subq_buf);
    return lsn;
}
