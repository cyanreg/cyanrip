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
#include "cyanrip_log.h"

#include <stdlib.h>
#include <string.h>

/* Max distance between a Q frame's absolute time and the sector read, same as cdrdao */
#define SUBQ_POSITION_TOLERANCE 20

static inline uint8_t bcd_to_bin(uint8_t x)
{
    return 10 * ((x & 0xF0) >> 4) + (x & 0x0F);
}

/* MMC-3 4.1.3.2.1: values from 0xA0 up are not BCD and are passed through */
static inline uint8_t subq_bcd_to_bin(uint8_t x)
{
    return x >= 0xA0 ? x : bcd_to_bin(x);
}

/* CRC-16/GSM over the first 10 bytes */
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

/* Offsets of the BCD fields of a mode 1 Q frame */
static const int subq_bcd_fields[] = { 1, 2, 3, 4, 5, 7, 8, 9 };
#define SUBQ_NB_BCD_FIELDS (sizeof(subq_bcd_fields) / sizeof(subq_bcd_fields[0]))

static void subq_bcd_fixup(uint8_t *subq_buf)
{
    for (size_t i = 0; i < SUBQ_NB_BCD_FIELDS; i++) {
        uint8_t x = subq_buf[subq_bcd_fields[i]];
        subq_buf[subq_bcd_fields[i]] = (uint8_t)(((x / 10) << 4) | (x % 10));
    }
}

/* Distance between a mode 1 frame's absolute time and lsn, -1 if invalid BCD */
static int subq_position_error(const uint8_t *subq_buf, const lsn_t lsn)
{
    for (size_t i = 0; i < SUBQ_NB_BCD_FIELDS; i++) {
        const uint8_t x = subq_buf[subq_bcd_fields[i]];
        if ((x >> 4) > 9 || (x & 0x0F) > 9)
            return -1;
    }

    subq_t subq;
    subq_decode(&subq, subq_buf);
    if (subq.track_number < 1 || subq.sec > 59 || subq.frame > 74 ||
        subq.asec > 59 || subq.aframe > 74)
        return -1;

    return abs(subq_abs_lsn(&subq) - lsn);
}

/* For drives without a Q CRC: accept a frame if its absolute time is close
 * to lsn, as BCD or as binary (subq_buf_fixed, copied over if it fits) */
static driver_return_code_t subq_validate_by_position(uint8_t *subq_buf, const uint8_t *subq_buf_fixed,
                                                      const lsn_t lsn)
{
    const uint8_t adr = subq_buf[0] & 0x0F;

    /* No position to check, the search steps over these anyway */
    if (adr == 2 || adr == 3)
        return DRIVER_OP_SUCCESS;
    if (adr != 1)
        return DRIVER_OP_ERROR;

    int is_binary = 1;
    for (size_t i = 0; i < SUBQ_NB_BCD_FIELDS; i++)
        is_binary &= subq_buf[subq_bcd_fields[i]] < 100;

    const int err_bcd = subq_position_error(subq_buf, lsn);
    const int err_bin = is_binary ? subq_position_error(subq_buf_fixed, lsn) : -1;
    const int bcd_fits = err_bcd >= 0 && err_bcd <= SUBQ_POSITION_TOLERANCE;
    const int bin_fits = err_bin >= 0 && err_bin <= SUBQ_POSITION_TOLERANCE;

    if (!bcd_fits && !bin_fits)
        return DRIVER_OP_ERROR;

    int use_binary = bin_fits;
    if (bcd_fits && bin_fits) {
        /* Take the closer one, a tie is only fine if both read the same */
        if (err_bcd == err_bin && memcmp(subq_buf, subq_buf_fixed, SUBQ_SIZE))
            return DRIVER_OP_ERROR;
        use_binary = err_bin < err_bcd;
    }

    if (use_binary)
        memcpy(subq_buf, subq_buf_fixed, SUBQ_SIZE);

    return DRIVER_OP_SUCCESS;
}

/* Q is bit 6 of each P-W byte */
static void subpw_extract_q(const uint8_t *pw_buf, uint8_t *subq_buf)
{
    uint8_t q[12] = { 0 };
    for (int i = 0; i < CDIO_CD_FRAMESIZE_SUB; i++)
        q[i >> 3] |= ((pw_buf[i] >> 6) & 1) << (7 - (i & 7));
    memset(subq_buf, 0, SUBQ_SIZE);
    memcpy(subq_buf, q, sizeof(q));
}

/* Leaves the Q frame at audio_subq_buf + CDIO_CD_FRAMESIZE_RAW in either mode */
static driver_return_code_t subq_read_sector(cyanrip_ctx *ctx, uint8_t *audio_subq_buf, const lsn_t lsn)
{
    if (ctx->subq_read_mode != CYANRIP_SUBQ_READ_RAW_PW)
        return cyanrip_read_audio_subchannel_sector(ctx->cdio, audio_subq_buf, lsn, CYANRIP_SUBCHANNEL_Q,
                                                    CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ);

    driver_return_code_t ret = cyanrip_read_audio_subchannel_sector(ctx->cdio, audio_subq_buf, lsn,
                                                                    CYANRIP_SUBCHANNEL_PW_RAW,
                                                                    CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW);
    if (ret)
        return ret;
    subpw_extract_q(audio_subq_buf + CDIO_CD_FRAMESIZE_RAW, audio_subq_buf + CDIO_CD_FRAMESIZE_RAW);
    return DRIVER_OP_SUCCESS;
}

void subq_probe_read_mode(cyanrip_ctx *ctx, uint8_t *audio_subq_buf,
                                 const lsn_t first_lsn, const lsn_t end_lsn)
{
    if (ctx->subq_read_mode != CYANRIP_SUBQ_READ_UNDETERMINED)
        return;

    if (ctx->settings.no_raw_subchannel) {
        ctx->subq_read_mode = CYANRIP_SUBQ_READ_FORMATTED_Q;
        return;
    }

    int nb_read = 0, nb_valid = 0;
    driver_return_code_t ret = DRIVER_OP_SUCCESS;
    for (lsn_t lsn = first_lsn; lsn < end_lsn && nb_read < SUBQ_PROBE_SECTORS; lsn++) {
        ret = cyanrip_read_audio_subchannel_sector(ctx->cdio, audio_subq_buf, lsn, CYANRIP_SUBCHANNEL_PW_RAW,
                                                   CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW);
        if (ret == DRIVER_OP_UNSUPPORTED)
            break;
        nb_read++;
        if (ret)
            continue;
        uint8_t subq_buf[SUBQ_SIZE];
        subpw_extract_q(audio_subq_buf + CDIO_CD_FRAMESIZE_RAW, subq_buf);
        nb_valid += subq_read_crc(subq_buf) == subq_crc(subq_buf);
    }

    ctx->subq_probe_frames = nb_read;
    ctx->subq_probe_valid_frames = nb_valid;
    if (ret != DRIVER_OP_UNSUPPORTED && nb_read > 0 && nb_valid * 2 >= nb_read) {
        ctx->subq_read_mode = CYANRIP_SUBQ_READ_RAW_PW;
        /* Raw frames are never converted, this also disables the position fallback */
        ctx->subq_bcd_fixup_status = CYANRIP_BCD_FIXUP_NOT_REQUIRED;
    } else {
        ctx->subq_read_mode = CYANRIP_SUBQ_READ_FORMATTED_Q;
    }
}

/* Some drives return binary instead of BCD, convert back if the CRC then matches */
static driver_return_code_t subq_read_valid_audio_sector(cyanrip_ctx *ctx, uint8_t *audio_subq_buf, const lsn_t lsn)
{
    driver_return_code_t ret = subq_read_sector(ctx, audio_subq_buf, lsn);
    if (ret)
        return ret;

    uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;
    if (ctx->subq_bcd_fixup_status == CYANRIP_BCD_FIXUP_REQUIRED) {
        subq_bcd_fixup(subq_buf);

        return (subq_read_crc(subq_buf) == subq_crc(subq_buf) ? DRIVER_OP_SUCCESS : DRIVER_OP_ERROR);
    }

    if (subq_read_crc(subq_buf) == subq_crc(subq_buf)) {
        ctx->subq_bcd_fixup_status = CYANRIP_BCD_FIXUP_NOT_REQUIRED;
        return DRIVER_OP_SUCCESS;
    }

    if (ctx->subq_bcd_fixup_status == CYANRIP_BCD_FIXUP_NOT_REQUIRED)
        return DRIVER_OP_ERROR;

    uint8_t subq_buf_copy[SUBQ_SIZE];
    memcpy(subq_buf_copy, subq_buf, SUBQ_SIZE);
    subq_bcd_fixup(subq_buf_copy);

    if (subq_read_crc(subq_buf_copy) == subq_crc(subq_buf_copy)) {
        ctx->subq_bcd_fixup_status = CYANRIP_BCD_FIXUP_REQUIRED;

        memcpy(subq_buf, subq_buf_copy, SUBQ_SIZE);
        return DRIVER_OP_SUCCESS;
    }

    /* No CRC has matched yet, the drive may not supply one */
    return subq_validate_by_position(subq_buf, subq_buf_copy, lsn);
}

driver_return_code_t subq_read_with_retries(cyanrip_ctx *ctx, uint8_t *audio_subq_buf,
    subq_t *subq, const lsn_t lsn, int *total_failures)
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

        if (ret != DRIVER_OP_ERROR || *total_failures > TOTAL_FAILURE_BUDGET)
            break;
    }

    return ret;
}

static int subq_repair_single_bit(uint8_t *subq_buf)
{
    for (int bit = 0; bit < 12 * 8; bit++) {
        subq_buf[bit >> 3] ^= 0x80 >> (bit & 7);
        if (subq_read_crc(subq_buf) == subq_crc(subq_buf))
            return 1;
        subq_buf[bit >> 3] ^= 0x80 >> (bit & 7);
    }
    return 0;
}

int subq_read_damaged(cyanrip_ctx *ctx, uint8_t *audio_subq_buf, subq_t *subq,
                             const lsn_t lsn, const track_t prev_track_number,
                             const track_t track_number, int *repaired)
{
    *repaired = 0;
    /* Formatted Q of a damaged sector is a substitute, not a damaged frame */
    if (ctx->subq_read_mode != CYANRIP_SUBQ_READ_RAW_PW)
        return 0;
    if (subq_read_sector(ctx, audio_subq_buf, lsn))
        return 0;

    uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;
    if (subq_read_crc(subq_buf) != subq_crc(subq_buf))
        *repaired = subq_repair_single_bit(subq_buf);

    if ((subq_buf[0] & 0x0F) != 1)
        return 0;
    if (subq_position_error(subq_buf, lsn) != 0)
        return 0;

    subq_decode(subq, subq_buf);
    return subq->track_number == prev_track_number || subq->track_number == track_number;
}
