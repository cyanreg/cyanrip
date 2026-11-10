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

driver_return_code_t cyanrip_read_audio_subchannel_sector(const CdIo_t *p_cdio, uint8_t *buf, const lsn_t lsn,
                                                          enum cyanrip_subchannel subchannel, size_t block_size)
{
    cdio_read_sub_t sub;
    switch (subchannel) {
    case CYANRIP_SUBCHANNEL_Q:      sub = CDIO_READ_SUB_Q;      break;
    case CYANRIP_SUBCHANNEL_PW_RAW: sub = CDIO_READ_SUB_PW_RAW; break;
    default:                        return DRIVER_OP_BAD_PARAMETER;
    }

    /* libcdio picks READ CD, the macOS ioctl or the disc image's own data */
    return cdio_read_audio_sectors_sub(p_cdio, buf, lsn, sub, 1);
}
