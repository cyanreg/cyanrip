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

#include "pregap.h"

#include <stdlib.h>
#include <stdint.h>

#include <cdio/cdio.h>
#include <cdio/mmc_ll_cmds.h>
// #include <cdio/iso9660.h>

#ifdef __APPLE__
#include <IOKit/storage/IOCDTypes.h>
#include <IOKit/storage/IOCDMediaBSDClient.h>
#include <sys/errno.h>
#endif


// Size of reads of audio + subchannel Q data. 2352 bytes for audio + 16 bytes for subchannel Q
#define CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ (CDIO_CD_FRAMESIZE_RAW + 16)


// TODO The implementation for macOS currently requires access to private
// internal cdio data, namely p_cdio->env.fd. Right now we just copy-paste some
// struct definitions to work around this. libcdio accepted a pull request for
// a function cdio_get_device_fd() (https://github.com/libcdio/libcdio/pull/37)
// that solves this issue. When that works its way into package managers it
// should be used here.
#ifdef __APPLE__
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


static driver_return_code_t read_audio_subq_sectors_mac(
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
#endif


static driver_return_code_t read_audio_subq_sector(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn)
{
    #ifdef __APPLE__
        return read_audio_subq_sectors_mac(p_cdio, audio_subq_buf, lsn, 1);
    #else
        return read_audio_subq_sectors_mmc(p_cdio, audio_subq_buf, lsn, 1);
    #endif
}


// #pragma pack(push, 1)
// typedef struct subq_encoded_t {
//     uint8_t  control:4;
//     uint8_t  adr    :4;
//     uint8_t  track_number;
//     uint8_t  index_number;
//     uint8_t  min;
//     uint8_t  sec;
//     uint8_t  frame;
//     uint8_t  zero;
//     uint8_t  amin;
//     uint8_t  asec;
//     uint8_t  aframe;
//     uint16_t crc;
//     uint8_t  reserved[4];
// } subq_encoded_t;
// #pragma pack(pop)

typedef struct subq_t {
    uint8_t  control;
    uint8_t  adr;
    uint8_t  track_number;
    uint8_t  index_number;
    uint8_t  min;
    uint8_t  sec;
    uint8_t  frame;
    uint8_t  amin;
    uint8_t  asec;
    uint8_t  aframe;
    unsigned crc;
} subq_t;

static inline uint8_t bcd_to_bin(uint8_t x){
    return 10*((x & 0xF0) >> 4) + (x & 0x0F);
}

// CRC-16/GSM with length 10
static inline unsigned crc_subq(const uint8_t* subq_buf)
{
    int length = 10;
    const unsigned crc_poly = 0x1021;
    unsigned r = 0x0000;
    while (length--) {
        r ^= *subq_buf++ << 8;
        for (int i = 0; i < 8; i++)
            r = r & 0x8000 ? (r << 1) ^ crc_poly : r << 1;
    }
    return ~r & 0xFFFF;
}

// MMC-3 Table 38 - Formatted Q sub-channel response data
static void decode_subq(subq_t *subq, const uint8_t *src) {
    subq->control       = (src[0] & 0xF0) >> 4;
    subq->adr           = (src[0] & 0x0F) >> 0;
    // TODO Unclear if these will always be BCD.  From MMC-3, the answer is yes.
    subq->track_number  = bcd_to_bin(src[1]);
    subq->index_number  = bcd_to_bin(src[2]);
    subq->min           = bcd_to_bin(src[3]);
    subq->sec           = bcd_to_bin(src[4]);
    subq->frame         = bcd_to_bin(src[5]);
    subq->amin          = bcd_to_bin(src[7]);
    subq->asec          = bcd_to_bin(src[8]);
    subq->aframe        = bcd_to_bin(src[9]);
    subq->crc           = (src[10] << 8) | src[11];
}


static driver_return_code_t read_audio_subq_sector_with_retries(
    const CdIo_t *p_cdio,
    uint8_t *audio_subq_buf,
    const lsn_t lsn,
    const int retry_max)
{
    driver_return_code_t ret = read_audio_subq_sector(p_cdio, audio_subq_buf, lsn);
    const uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;
    unsigned crc_read = (subq_buf[10] << 8) | subq_buf[11];
    unsigned crc_comp = crc_subq(subq_buf);
    int retry = 0;
    while (retry++ < retry_max && crc_read != crc_comp) {
        // TODO Is a cache defeat here ever necessary? Testing on macOS with an
        // ASUS SDRW-08U7M-U, it didn't have an effect.
        // overflow_device_read_cache(p_cdio, lsn);
        if ((ret = read_audio_subq_sector(p_cdio, audio_subq_buf, lsn)))
            return ret;
        // TODO ret error handling
        crc_read = (subq_buf[10] << 8) | subq_buf[11];
        crc_comp = crc_subq(subq_buf);
    }
    return ret;
}


lsn_t cyanrip_get_track_pregap_lsn(CdIo_t *p_cdio, const track_t track_number) {
    // Try to use libcdio. If libcdio doesn't implement pregap finding
    // for a driver, it will return CDIO_INVALID_LSN.
    const lsn_t cdio_track_pregap_lsn = cdio_get_track_pregap_lsn(p_cdio, track_number);
    if (cdio_track_pregap_lsn != CDIO_INVALID_LSN)
        return cdio_track_pregap_lsn;

    // First track pregap is lsn = 0, lba = CDIO_PREGAP_SECTORS.
    // TODO Under what circumstances does libcdio give a first track not equal
    // to 1? Does this ever happen for a rippable CD?
    const track_t first_track_number = cdio_get_first_track_num(p_cdio);
    if (track_number == first_track_number)
        return 0;

    const lsn_t track_start_lsn = cdio_get_track_lsn(p_cdio, track_number);
    // TODO Is (track_number - 1) always safe? e.g. non-continuous track numbers?
    const uint8_t prev_track_number = track_number - 1;
    const lsn_t prev_track_start_lsn = cdio_get_track_lsn(p_cdio, prev_track_number);

    // Handle single sector previous track.
    if (prev_track_start_lsn + 1 == track_start_lsn)
        return track_start_lsn;

    uint8_t *audio_subq_buf = malloc(CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ);
    const uint8_t *subq_buf = audio_subq_buf + CDIO_CD_FRAMESIZE_RAW;

    lsn_t lsn;
    subq_t subq;
    unsigned crc_comp;
    int retry_max = 5;
    const int harder_retry_max = 100;
    driver_return_code_t ret;

    lsn_t left_bound = prev_track_start_lsn;
    lsn_t right_bound = track_start_lsn;

    // Check one sector before track start to see if there is any pregap.
    lsn = track_start_lsn - 1;
    ret = read_audio_subq_sector_with_retries(p_cdio, audio_subq_buf, lsn, retry_max);
    decode_subq(&subq, subq_buf);
    crc_comp = crc_subq(subq_buf);
    if (subq.crc == crc_comp && subq.adr == 1 && subq.track_number == prev_track_number)
        return track_start_lsn;

    if (subq.crc == crc_comp && subq.adr == 1 && subq.track_number == track_number)
        right_bound = lsn;

    // There is a pregap or the result was ambiguous. Backtrack in 2
    // second increments until we find a position that can be confirmed to be
    // before any pregap. A 2 second pregap is common so this will often
    // backtrack right to the pregap boundary.
    const lsn_t backtrack = 150;
    for (;;) {
        lsn = lsn - backtrack >= prev_track_start_lsn ? lsn - backtrack : prev_track_start_lsn;
        if (lsn == prev_track_start_lsn) {
            break;
        }
        ret = read_audio_subq_sector_with_retries(p_cdio, audio_subq_buf, lsn, retry_max);
        decode_subq(&subq, subq_buf);
        crc_comp = crc_subq(subq_buf);
        if (subq.crc != crc_comp || subq.adr != 1) {
            continue;
        }
        else if (subq.track_number == track_number) {
            right_bound = lsn;
        }
        else {
            break;
        }
    }
    left_bound = lsn;

    // Loop over sectors from left bound to right bound attempting to contract bounds until
    // they meet. This allows us to skip over sectors with bad CRCs and only come back to
    // them if necessary.
    while ((left_bound + 1) != right_bound) {
        lsn += 1;
        if (lsn == right_bound) {
            // Skipping over sectors with bad CRCs failed.
            // If we've already been here, give up.
            if (retry_max == harder_retry_max)
                break;
            // If this is the first time, try harder.
            retry_max = harder_retry_max;
            lsn = left_bound;
            continue;
        }
        ret = read_audio_subq_sector_with_retries(p_cdio, audio_subq_buf, lsn, retry_max);
        decode_subq(&subq, subq_buf);
        crc_comp = crc_subq(subq_buf);
        if (subq.crc != crc_comp) {
            // Attempt to skip over sectors with bad CRCs.
            continue;
        }
        else if (subq.adr != 1) {
            // If a mode 2 or mode 3 sector immediately follows left bound,
            // consider it part of previous track and contract left bound.
            if (lsn - 1 == left_bound)
                left_bound = lsn;
        }
        else if (subq.track_number == prev_track_number) {
            left_bound = lsn;
        }
        else if (subq.track_number == track_number) {
            right_bound = lsn;
            // Restart loop.
            lsn = left_bound;
        }
    }
    // TODO Error reporting for failing to find pregap due to CRC mismatches.
    lsn = (left_bound + 1 == right_bound) ? right_bound : DRIVER_OP_ERROR;

    free(audio_subq_buf);
    return lsn;
}
