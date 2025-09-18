#include "pregap.h"

#include <stdlib.h>
#include <stdint.h>

#include <cdio/cdio.h>
#include <cdio/iso9660.h>
#include <cdio/mmc_ll_cmds.h>

#ifdef __APPLE__
#include <IOKit/storage/IOCDTypes.h>
#include <IOKit/storage/IOCDMediaBSDClient.h>
#endif


// TODO The implementation for macOS requires access to private internal cdio
// data, namely p_cdio->env.fd. Right now we just copy-paste some struct
// definitions to work around this. Is there a less brittle way to do do this? :-/

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
    track_t i_first_track;
    track_t i_tracks;
    uint8_t u_joliet_level;
    iso9660_pvd_t pvd;
    iso9660_svd_t svd;
    CdIo_t   *cdio;
    cdtext_t *cdtext;
    track_flags_t track_flags[CDIO_CD_MAX_TRACKS+1];
    unsigned char  scsi_mmc_sense[263];
    int            scsi_mmc_sense_valid;
    char *scsi_tuple;
} generic_img_private_t;


#pragma pack(push, 1)
typedef struct subq_t {
    uint8_t  adr    :4;
    uint8_t  control:4;
    uint8_t  track_number;
    uint8_t  index_number;
    uint8_t  min;
    uint8_t  sec;
    uint8_t  frame;
    uint8_t  zero;
    uint8_t  amin;
    uint8_t  asec;
    uint8_t  aframe;
    uint16_t crc16;
    uint8_t  reserved[4];
} subq_t;
#pragma pack(pop)

typedef struct user_subq_t {
    int16_t samples[1176];
    subq_t subq;
} user_subq_t;


static driver_return_code_t get_lsn_index_mmc(
    CdIo_t *p_cdio,
    const lsn_t i_lsn,
    user_subq_t *p_user_subq,
    uint8_t *p_index)
{
    const int expected_sector_type = 1; /* CD-DA sectors */
    const bool b_digital_audio_play = false;
    const bool b_sync = false;
    const uint8_t header_codes = 0; /* no header information */
    const bool b_user_data = true;
    const bool b_edc_ecc = false;
    const uint8_t c2_error_information = 0;
    const uint8_t subchannel_selection = 2; /* Q sub-channel */
    const uint16_t i_blocksize = CDIO_CD_FRAMESIZE_RAW + 16;
    const uint32_t i_blocks = 1;

    driver_return_code_t rc =
    mmc_read_cd(p_cdio, p_user_subq, i_lsn, expected_sector_type,
        b_digital_audio_play, b_sync, header_codes, b_user_data, b_edc_ecc,
        c2_error_information, subchannel_selection, i_blocksize, i_blocks);
    if (rc) {
        return rc;
    }
    else {
        *p_index = p_user_subq->subq.index_number;
        return rc;
    }
}

#ifdef __APPLE__
static driver_return_code_t get_lsn_index_mac(
    CdIo_t *p_cdio,
    const lsn_t i_lsn,
    user_subq_t *p_user_subq,
    uint8_t *p_index)
{
    generic_img_private_t *p_gen = (generic_img_private_t*)(p_cdio->env);
    const int fd = p_gen->fd;

    const CDSectorSize cdda_sect_size_user = 2352;
    const CDSectorSize cdda_sect_size_subq = 16;
    const CDSectorSize sect_read_size = cdda_sect_size_user + cdda_sect_size_subq;
    dk_cd_read_t cd_read = {
        .offset = i_lsn*sect_read_size,
        .sectorArea = kCDSectorAreaUser | kCDSectorAreaSubChannelQ,
        .sectorType = kCDSectorTypeCDDA,
        .bufferLength = sizeof(user_subq_t),
        .buffer = p_user_subq,
    };
    int err = ioctl(fd, DKIOCCDREAD, &cd_read);
    if (err)
        return DRIVER_OP_ERROR;

    const uint8_t index = p_user_subq->subq.index_number;
    *p_index = index;
    return DRIVER_OP_SUCCESS;
}
#endif


static driver_return_code_t get_lsn_index(
    CdIo_t *p_cdio,
    const lsn_t i_lsn,
    user_subq_t *user_subq,
    uint8_t *index_p)
{
    #ifdef __APPLE__
        return get_lsn_index_mac(p_cdio, i_lsn, user_subq, index_p);
    #else
        return get_lsn_index_mmc(p_cdio, i_lsn, user_subq, index_p);
    #endif
}


lsn_t crip_get_track_pregap_lsn(CdIo_t *p_cdio, track_t i_track) {
    // TODO more error checking and reporting?

    // Use libcdio implementation, if it exists.
    if (p_cdio->op.get_track_pregap_lba)
        return p_cdio->op.get_track_pregap_lba (p_cdio->env, i_track);

    // First track pregap is the start of the user area, lsn = 0.
    const track_t first_track = cdio_get_first_track_num(p_cdio);
    if (i_track == first_track)
        return 0;

    // Is there a libcdio method for previous track? I'm not 100% sure it's
    // always safe to just subtract 1.
    const lsn_t i_prev_index1 = cdio_get_track_lsn(p_cdio, i_track - 1);

    // First check one sector before track start to see if there is any
    // pregap at all.
    const lsn_t i_track_lsn = cdio_get_track_lsn(p_cdio, i_track);
    lsn_t i_lsn = i_track_lsn - 1;
    const uint16_t i_blocksize = CDIO_CD_FRAMESIZE_RAW + 16;
    user_subq_t *p_user_subq = malloc(i_blocksize);

    uint8_t index;
    driver_return_code_t rc;
    rc = get_lsn_index(p_cdio, i_lsn, p_user_subq, &index);
    if (rc) {
        free(p_user_subq);
        return CDIO_INVALID_LBA;
    }
    if (index != 0) {
        free(p_user_subq);
        return i_track_lsn;
    }

    // There is a pregap. Backtrack in 2 second increments until we're before
    // the start of the pregap. A 2 second pregap is common so this will often
    // backtrack to the exact index boundary.
    while (index == 0 && i_lsn > i_prev_index1) {
        const lsn_t backtrack = 150;
        if (i_lsn < backtrack)
            i_lsn = 0;
        else
            i_lsn -= backtrack;
        rc = get_lsn_index(p_cdio, i_lsn, p_user_subq, &index);
        if (rc) {
            free(p_user_subq);
            return CDIO_INVALID_LSN;
        }
    }

    // Check if we backtracked all the way to the start of the previous track.
    // This shouldn't happen on a valid disc.
    if (index == 0) {
        // error, entire track of index
        free(p_user_subq);
        return CDIO_INVALID_LSN;
    }

    // Scan forward one sector at a time to find start of pregap.
    while (index != 0) {
        i_lsn += 1;
        rc = get_lsn_index(p_cdio, i_lsn, p_user_subq, &index);
        if (rc) {
            free(p_user_subq);
            return CDIO_INVALID_LBA;
        }
    }

    free(p_user_subq);
    return i_lsn;
}
