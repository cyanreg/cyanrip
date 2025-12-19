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

// TODO The implementation for macOS currently requires access to private
// internal cdio data, namely p_cdio->env.fd. Right now we just copy-paste some
// struct definitions to work around this. libcdio accepted a pull request for
// a function cdio_get_device_fd() (https://github.com/libcdio/libcdio/pull/37)
// that solves this issue. When that works its way into package managers it
// should be used here.
// lib/driver/mmc/mmc_private.h
typedef driver_return_code_t (*mmc_run_cmd_fn_t) 
     ( void *p_user_data, 
       unsigned int i_timeout_ms,
       unsigned int i_cdb, 
       const mmc_cdb_t *p_cdb, 
       cdio_mmc_direction_t e_direction, 
       unsigned int i_buf, void *p_buf);

// lib/driver/cdio_private.h
typedef struct {
    driver_return_code_t (*audio_get_volume)
         (void *p_env,  /*out*/ cdio_audio_volume_t *p_volume);
    driver_return_code_t (*audio_pause) (void *p_env);
    driver_return_code_t (*audio_play_msf) ( void *p_env,
                                             msf_t *p_start_msf,
                                             msf_t *p_end_msf );
    driver_return_code_t (*audio_play_track_index)
         ( void *p_env, cdio_track_index_t *p_track_index );
    driver_return_code_t (*audio_read_subchannel)
         ( void *p_env, cdio_subchannel_t *subchannel );
    driver_return_code_t (*audio_resume) ( void *p_env );
    driver_return_code_t (*audio_set_volume)
         ( void *p_env,  cdio_audio_volume_t *p_volume );
    driver_return_code_t (*audio_stop) ( void *p_env );
    driver_return_code_t (*eject_media) ( void *p_env );
    void (*free) (void *p_env);
    const char * (*get_arg) (void *p_env, const char key[]);
    int (*get_blocksize) ( void *p_env );
    cdtext_t * (*get_cdtext) ( void *p_env );
    uint8_t * (*get_cdtext_raw) ( void *p_env );
    char ** (*get_devices) ( void );
    char * (*get_default_device) ( void );
    lsn_t (*get_disc_last_lsn) ( void *p_env );
    discmode_t (*get_discmode) ( void *p_env );
    void (*get_drive_cap) (const void *p_env,
                           cdio_drive_read_cap_t  *p_read_cap,
                           cdio_drive_write_cap_t *p_write_cap,
                           cdio_drive_misc_cap_t  *p_misc_cap);
    track_t (*get_first_track_num) ( void *p_env );
    bool (*get_hwinfo)
         ( const CdIo_t *p_cdio, /* out*/ cdio_hwinfo_t *p_hw_info );
    driver_return_code_t (*get_last_session)
         ( void *p_env, /*out*/ lsn_t *i_last_session );
    int (*get_media_changed) ( const void *p_env );
    char * (*get_mcn) ( const void *p_env );
    track_t (*get_num_tracks) ( void *p_env );
    int (*get_track_channels) ( const void *p_env, track_t i_track );
    track_flag_t (*get_track_copy_permit) ( void *p_env, track_t i_track );
    lba_t (*get_track_lba) ( void *p_env, track_t i_track );
    lba_t (*get_track_pregap_lba) ( const void *p_env, track_t i_track );
    char * (*get_track_isrc) ( const void *p_env, track_t i_track );
    track_format_t (*get_track_format) ( void *p_env, track_t i_track );
    bool (*get_track_green) ( void *p_env, track_t i_track );
    bool (*get_track_msf) ( void *p_env, track_t i_track, msf_t *p_msf );
    track_flag_t (*get_track_preemphasis)
         ( const void  *p_env, track_t i_track );
    off_t (*lseek) ( void *p_env, off_t offset, int whence );
    ssize_t (*read) ( void *p_env, void *p_buf, size_t i_size );
    int (*read_audio_sectors) ( void *p_env, void *p_buf, lsn_t i_lsn,
                                unsigned int i_blocks );
    driver_return_code_t (*read_data_sectors)
         ( void *p_env, void *p_buf, lsn_t i_lsn, uint16_t i_blocksize,
           uint32_t i_blocks );
    int (*read_mode2_sector)
         ( void *p_env, void *p_buf, lsn_t i_lsn, bool b_mode2_form2 );
    int (*read_mode2_sectors)
         ( void *p_env, void *p_buf, lsn_t i_lsn, bool b_mode2_form2,
           unsigned int i_blocks );
    int (*read_mode1_sector)
         ( void *p_env, void *p_buf, lsn_t i_lsn, bool mode1_form2 );
    int (*read_mode1_sectors)
         ( void *p_env, void *p_buf, lsn_t i_lsn, bool mode1_form2,
           unsigned int i_blocks );
    bool (*read_toc) ( void *p_env ) ;
    mmc_run_cmd_fn_t run_mmc_cmd;
    int (*set_arg) ( void *p_env, const char key[], const char value[] );
    driver_return_code_t (*set_blocksize) ( void *p_env,
                                            uint16_t i_blocksize );
    int (*set_speed) ( void *p_env, int i_speed );
} cdio_funcs_t;

// lib/driver/cdio_private.h
typedef struct {
    uint16_t    u_type;
    uint16_t    u_flags;
} cdio_header_t;

// lib/driver/cdio_private.h
struct _CdIo {
    cdio_header_t header;
    driver_id_t   driver_id;
    cdio_funcs_t  op;
    void*         env;
};

// lib/driver/generic.h
typedef struct {
    char *source_name;
    bool  init;
    bool  toc_init;
    bool  b_cdtext_error;
    int   ioctls_debugged;
    void *data_source;
    int     fd;
    // track_t i_first_track;
    // track_t i_tracks;
    // uint8_t u_joliet_level;
    // iso9660_pvd_t pvd;
    // iso9660_svd_t svd;
    // CdIo_t   *cdio;
    // cdtext_t *cdtext;
    // track_flags_t track_flags[CDIO_CD_MAX_TRACKS+1];
    // unsigned char  scsi_mmc_sense[263];
    // int            scsi_mmc_sense_valid;
    // char *scsi_tuple;
} generic_img_private_t;


driver_return_code_t cyanrip_read_audio_subq_sectors(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn,
    const uint32_t blocks)
{
    generic_img_private_t *p_gen = (generic_img_private_t*)(p_cdio->env);
    const int fd = p_gen->fd;

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
    const int ioctl_errno = errno;
    // TODO More detailed error handling? errno will be one of:
    // EBADF
    // EINVAL
    // ENOTTY
    // printf("ioctl() errno: %d\n", ioctl_errno);
    return DRIVER_OP_ERROR;
}
