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

#include "cyanrip_main.h"
#include "libavutil/crc.h"

typedef struct cyanrip_crc_ctx {
    const AVCRC *eac_ctx;
    uint32_t eac_crc;
    uint32_t acu_start;
    uint32_t acu_end;
    uint32_t acu_mult;
    uint32_t acu_sum_1;
    uint32_t acu_sum_2;
} cyanrip_crc_ctx;

static inline void init_crc_ctx(cyanrip_ctx *ctx, cyanrip_crc_ctx *s, cyanrip_track *t)
{
    s->eac_ctx   = av_crc_get_table(AV_CRC_32_IEEE_LE);
    s->eac_crc   = UINT32_MAX;
    s->acu_start = 0;
    s->acu_end   = t->nb_samples >> 1;
    s->acu_mult  = 1;
    s->acu_sum_1 = 0x0;
    s->acu_sum_2 = 0x0;

    t->computed_crcs = 0;

    if (t->number == 1)
        s->acu_start += (CDIO_CD_FRAMESIZE_RAW*5) >> 2;
    else if (t->number == ctx->drive->tracks)
        s->acu_end   -= (CDIO_CD_FRAMESIZE_RAW*5) >> 2;
}

static inline void process_crc(cyanrip_crc_ctx *s, const uint8_t *data, int bytes)
{
    if (!bytes)
        return;

    s->eac_crc = av_crc(s->eac_ctx, s->eac_crc, data, bytes);

    for (int j = 0; j < (bytes >> 2); j++) {
        if (s->acu_mult >= s->acu_start && s->acu_mult <= s->acu_end) {
            uint32_t val = AV_RL32(&data[j*4]);
            uint64_t tmp = (uint64_t)val  * (uint64_t)s->acu_mult;
            uint32_t lo  = (uint32_t)(tmp & (uint64_t)UINT32_MAX);
            uint32_t hi  = (uint32_t)(tmp / (uint64_t)0x100000000);
            s->acu_sum_1 += s->acu_mult * val;
            s->acu_sum_2 += hi;
            s->acu_sum_2 += lo;
        }
        s->acu_mult++;
    }
}

static inline void finalize_crc(cyanrip_crc_ctx *s, cyanrip_track *t)
{
    t->computed_crcs = 1;
    t->eac_crc = s->eac_crc ^ UINT32_MAX;
    t->acurip_crc_v1 = s->acu_sum_1;
    t->acurip_crc_v2 = s->acu_sum_2;
}
