/*
 * Copyright (C) 2016 Rostislav Pehlivanov <atomnuker@gmail.com>
 *
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

#include "cyanrip_crc.h"

static uint32_t cyanrip_crc_v1(cyanrip_ctx *ctx)
{
    int mult = 1;
    int start = 0;
    int end   = ctx->samples_num/2;

    uint32_t sum = 0;

    if (ctx->cur_track == 1)
        start += (CDIO_CD_FRAMESIZE_RAW*5)/4;
    else if (ctx->cur_track == ctx->drive->tracks)
        end   -= (CDIO_CD_FRAMESIZE_RAW*5)/4;

    for (int i = 0; i < ctx->samples_num/2; i++) {
		if (mult >= start && mult <= end) 
			sum += mult * ctx->samples[i];

		mult++;
	}

    return sum;
}

static uint32_t cyanrip_crc_v2(cyanrip_ctx *ctx)
{
    int mult = 1;
    int start = 0;
    int end   = ctx->samples_num/2;
    uint32_t *samples = (uint32_t *)ctx->samples;

    uint32_t sum = 0;

    if (ctx->cur_track == 1)
        start += (CDIO_CD_FRAMESIZE_RAW*5)/4;
    else if (ctx->cur_track == ctx->drive->tracks)
        end   -= (CDIO_CD_FRAMESIZE_RAW*5)/4;


    for (int i = 0; i < ctx->samples_num/2; i++) {
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


int cyanrip_crc_track(cyanrip_ctx *ctx)
{
    ctx->tracks[ctx->cur_track].crc_v1 = cyanrip_crc_v1(ctx);
    ctx->tracks[ctx->cur_track].crc_v2 = cyanrip_crc_v2(ctx);

    return 0;
}
