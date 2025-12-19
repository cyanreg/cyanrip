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

#include <stdint.h>
#include <cdio/cdio.h>

// Size of reads of audio + subchannel Q data. 2352 bytes for audio + 16 bytes for subchannel Q
#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ (CDIO_CD_FRAMESIZE_RAW + 16)

driver_return_code_t cyanrip_read_audio_subq_sectors(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn,
    const uint32_t blocks);
