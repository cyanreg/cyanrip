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

#define SUBQ_SIZE 16

/* Size of reads of audio + subchannel Q data. 2352 bytes for audio + 16 bytes for subchannel Q */
#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ (CDIO_CD_FRAMESIZE_RAW + SUBQ_SIZE)

/* Size of reads of audio + raw P-W subchannel data. 2352 bytes for audio + 96 bytes of P-W */
#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW (CDIO_CD_FRAMESIZE_RAW + CDIO_CD_FRAMESIZE_SUB)

/**
 * Reads audio + subchannel Q data from a CD device
 * into audio_subq_buf.
 * 
 * The buffer must be large enough to hold CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ bytes.
 * 
 * Note: The Subchannel Q data still needs to be verified for CRC validity after reading.
 */
driver_return_code_t cyanrip_read_audio_subq_sector(const CdIo_t *p_cdio, uint8_t *audio_subq_buf, const lsn_t lsn);

/**
 * Reads audio + raw P-W subchannel data from a CD device into audio_subpw_buf.
 *
 * The buffer must be large enough to hold CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW bytes.
 *
 * Unlike the formatted Q sub-channel above, the 96 bytes are the subcode
 * symbols as read off the disc: bit 7 of each is the P channel, bit 6 the Q
 * channel and so on down to bit 0 for W. The drive neither checks the Q CRC
 * nor substitutes anything, so what comes back is what the sector holds.
 *
 * Returns DRIVER_OP_UNSUPPORTED where the backend can't read raw P-W.
 */
driver_return_code_t cyanrip_read_audio_subpw_sector(const CdIo_t *p_cdio, uint8_t *audio_subpw_buf, const lsn_t lsn);
