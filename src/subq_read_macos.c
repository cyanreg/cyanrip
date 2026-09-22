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

#include <IOKit/storage/IOCDTypes.h>
#include <IOKit/storage/IOCDMediaBSDClient.h>
#include <sys/errno.h>
#include <cdio/cdio.h>

/**
 * Reads a sector's audio plus the sub-channel area given by sector_area
 * (kCDSectorAreaSubChannelQ for the formatted 16 byte Q, kCDSectorAreaSubChannel
 * for the 96 raw P-W symbols) into buf, which must hold block_size bytes.
 */
static driver_return_code_t read_audio_sector_with_subchannel(const CdIo_t *p_cdio, uint8_t *buf,
                                                              const lsn_t lsn, const uint8_t sector_area,
                                                              const unsigned block_size)
{
    const int fd = cdio_get_device_fd((CdIo_t *)p_cdio);
    if (fd < 0) {
        return DRIVER_OP_ERROR;
    }

    dk_cd_read_t cd_read = {
        .offset = block_size*lsn,
        .sectorArea = kCDSectorAreaUser | sector_area,
        .sectorType = kCDSectorTypeCDDA,
        .bufferLength = block_size,
        .buffer = buf,
    };
    if (ioctl(fd, DKIOCCDREAD, &cd_read) >= 0)
        return DRIVER_OP_SUCCESS;

    /* Map the ioctl() failure to the closest driver_return_code_t, so that
     * callers can retry on errors that are actually transient, i.e. DRIVER_OP_ERROR.
     */
    switch (errno) {
        case EBADF:  /* fd is invalid, e.g. the device was already closed */
            return DRIVER_OP_UNINIT;
        case EINVAL: /* Invalid argument, e.g. bad offset/buffer length */
            return DRIVER_OP_BAD_PARAMETER;
        case ENOTTY: /* DKIOCCDREAD is not supported on this fd/device */
            return DRIVER_OP_UNSUPPORTED;
        default:     /* Most likely a transient read error (e.g. EIO), retryable */
            return DRIVER_OP_ERROR;
    }
}

driver_return_code_t cyanrip_read_audio_subq_sector(const CdIo_t *p_cdio, uint8_t *audio_subq_buf, const lsn_t lsn)
{
    return read_audio_sector_with_subchannel(p_cdio, audio_subq_buf, lsn, kCDSectorAreaSubChannelQ,
                                             CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ);
}

driver_return_code_t cyanrip_read_audio_subpw_sector(const CdIo_t *p_cdio, uint8_t *audio_subpw_buf, const lsn_t lsn)
{
    return read_audio_sector_with_subchannel(p_cdio, audio_subpw_buf, lsn, kCDSectorAreaSubChannel,
                                             CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW);
}
