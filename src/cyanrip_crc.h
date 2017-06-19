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

static inline uint32_t ieee_crc_32(cyanrip_ctx *ctx, cyanrip_track *t)
{
    const AVCRC *avcrc = av_crc_get_table(AV_CRC_32_IEEE);
    return av_crc(avcrc, 0, (uint8_t *)t->samples, t->nb_samples << 1);
}

static inline uint32_t eac_crc_32(cyanrip_ctx *ctx, cyanrip_track *t)
{
    const AVCRC *avcrc = av_crc_get_table(AV_CRC_32_IEEE_LE);
    uint32_t crc = 0xFFFFFFFF;
    crc = av_crc(avcrc, crc, (uint8_t *)t->samples, t->nb_samples << 1);
    crc ^= 0xFFFFFFFF;
    return crc;
}

static inline uint32_t acurip_crc_v1(cyanrip_ctx *ctx, cyanrip_track *t)
{
    int mult = 1;
    int start = 0;
    int end   = t->nb_samples/2;
    uint32_t *samples = (uint32_t *)t->samples;

    uint32_t sum = 0;

    if (!t->index)
        start += (CDIO_CD_FRAMESIZE_RAW*5)/4;
    else if (t->index == ctx->drive->tracks - 1)
        end   -= (CDIO_CD_FRAMESIZE_RAW*5)/4;

    for (int i = 0; i < t->nb_samples; i++) {
		if (mult >= start && mult <= end)
			sum += mult * samples[i];

		mult++;
	}

    return sum;
}

static inline uint32_t acurip_crc_v2(cyanrip_ctx *ctx, cyanrip_track *t)
{
    int mult = 1;
    int start = 0;
    int end   = t->nb_samples/2;
    uint32_t *samples = (uint32_t *)t->samples;

    uint32_t sum = 0;

    if (!t->index)
        start += (CDIO_CD_FRAMESIZE_RAW*5)/4;
    else if (t->index == ctx->drive->tracks - 1)
        end   -= (CDIO_CD_FRAMESIZE_RAW*5)/4;

    for (int i = 0; i < t->nb_samples >> 1; i++) {
		if (mult >= start && mult <= end) {
		    uint32_t val = samples[i];
			uint64_t tmp = (uint64_t)val  * (uint64_t)mult;
            uint32_t lo  = (uint32_t)(tmp & (uint64_t)0xFFFFFFFF);
            uint32_t hi  = (uint32_t)(tmp / (uint64_t)0x100000000);
            sum += hi;
            sum += lo;
	    }

		mult++;
	}

    return sum;
}

static inline int cyanrip_crc_track(cyanrip_ctx *ctx, cyanrip_track *t)
{
    t->ieee_crc_32 = ieee_crc_32(ctx, t);
    t->eac_crc     = eac_crc_32(ctx, t);
    t->acurip_crc_v1 = acurip_crc_v1(ctx, t);
    t->acurip_crc_v2 = acurip_crc_v2(ctx, t);

    return 0;
}
