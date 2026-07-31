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

driver_return_code_t cyanrip_read_audio_subq_sectors(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn,
    const uint32_t blocks)
{
    const int fd = cdio_get_device_fd((Cdio_t *)p_cdio);
    if (fd < 0) {
        return DRIVER_OP_ERROR;
    }

    const unsigned block_size = CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ;
    dk_cd_read_t cd_read = {
        .offset = block_size*lsn,
        .sectorArea = kCDSectorAreaUser | kCDSectorAreaSubChannelQ,
        .sectorType = kCDSectorTypeCDDA,
        .bufferLength = block_size*blocks,
        .buffer = audio_subq_buf,
    };
    if (!ioctl(fd, DKIOCCDREAD, &cd_read))
        return DRIVER_OP_SUCCESS;

    // TODO More detailed error handling? errno will be one of:
    // EBADF
    // EINVAL
    // ENOTTY
    // printf("ioctl() errno: %d\n", ioctl_errno);
    return DRIVER_OP_ERROR;
}
