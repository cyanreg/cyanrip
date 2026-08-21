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

driver_return_code_t cyanrip_read_audio_subq_sector(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn)
{
    const int fd = cdio_get_device_fd((CdIo_t *)p_cdio);
    if (fd < 0) {
        return DRIVER_OP_ERROR;
    }

    const unsigned block_size = CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ;
    dk_cd_read_t cd_read = {
        .offset = block_size*lsn,
        .sectorArea = kCDSectorAreaUser | kCDSectorAreaSubChannelQ,
        .sectorType = kCDSectorTypeCDDA,
        .bufferLength = block_size,
        .buffer = audio_subq_buf,
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
