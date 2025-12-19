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

#include <cdio/mmc_ll_cmds.h>

driver_return_code_t cyanrip_read_audio_subq_sectors(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn,
    const uint32_t blocks)
{
    const int expected_sector_type = 1; /* CD-DA sectors */
    const bool b_digital_audio_play = false;
    const bool b_sync = false;
    const uint8_t header_codes = 0; /* no header information */
    const bool b_user_data = true;
    const bool b_edc_ecc = false;
    const uint8_t c2_error_information = 0;
    const uint8_t subchannel_selection = 2; /* Q sub-channel */
    const uint16_t i_blocksize = CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ;

    return mmc_read_cd(p_cdio, audio_subq_buf, lsn, expected_sector_type,
        b_digital_audio_play, b_sync, header_codes, b_user_data, b_edc_ecc,
        c2_error_information, subchannel_selection, i_blocksize, blocks);
}
