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

/* "Accurip checksums"? WHAT CHECKSUMS? THEY'RE NOT DESERVING OF BEING CALLED CHECKSUMS!
 * Checksums check things in the most optimal way in order to detect bit errors in completely
 * arbitrary positions. What a checksum is not, however, is all samples summed together.
 * Which is what theses so called "Accuraterip checksums" are. They're COMPLETELY UNSCIENTIFIC,
 * algorithms without any REAL MATHEMATICAL BASIS for error detection. They're complete crap
 * and the person who wrote them HAD ABSOLUTELY NO FUCKING CLUE WHAT A FUCKING CHECKSUM IS.
 * Below is a real checksum. Below is something way more optimal. Below is an IEEE 32 bit
 * checksum with a polynomial. A real checksum. The polynomial may not best fit the distribution
 * of CD data but nevertheless IT FUCKING CHECKS THINGS ARE OKAY AND WILL DETECT INACCURACIES!
 * Not that you need to know that since the program will warn on errors.
 * I'm a person who had to pour his blood, sweat and patience to implement an 18 bit CRC
 * on 10 bit data sources, so I have the dignity of NOT implementing that crap.
 * If someone else wants to do it instead, the data is in t->samples, just be sure to offset
 *it below like in the function which calculates a true checksum. */

static inline uint32_t ieee_crc_32(cyanrip_ctx *ctx, cyanrip_track *t)
{
    const AVCRC *avcrc = av_crc_get_table(AV_CRC_32_IEEE);
    int16_t *samples = t->samples + (OVER_UNDER_READ_FRAMES*CDIO_CD_FRAMESIZE_RAW >> 1) + ctx->settings.offset*2;
    return av_crc(avcrc, 0, (uint8_t *)samples, t->nb_samples << 1);
}

static inline int cyanrip_crc_track(cyanrip_ctx *ctx, cyanrip_track *t)
{
    t->ieee_crc_32 = ieee_crc_32(ctx, t);

    return 0;
}
