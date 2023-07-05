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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../config.h"
#include "version.h"

#include "utils.h"

#include <cdio/paranoia/paranoia.h>
#include <cdio/audio.h>
#include <libavutil/mem.h>
#include <libavutil/dict.h>
#include <libavutil/avstring.h>
#include <libavutil/intreadwrite.h>
#include <libavcodec/avcodec.h>

enum cyanrip_output_formats {
    CYANRIP_FORMAT_FLAC = 0,
    CYANRIP_FORMAT_TTA,
    CYANRIP_FORMAT_OPUS,
    CYANRIP_FORMAT_AAC,
    CYANRIP_FORMAT_WAVPACK,
    CYANRIP_FORMAT_ALAC,
    CYANRIP_FORMAT_MP3,
    CYANRIP_FORMAT_VORBIS,
    CYANRIP_FORMAT_WAV,
    CYANRIP_FORMAT_AAC_MP4,
    CYANRIP_FORMAT_OPUS_MP4,
    CYANRIP_FORMAT_PCM,

    CYANRIP_FORMATS_NB,
};

enum cyanrip_pregap_action {
    CYANRIP_PREGAP_DEFAULT = 0,
    CYANRIP_PREGAP_DROP,
    CYANRIP_PREGAP_MERGE,
    CYANRIP_PREGAP_TRACK,
};

enum CRIPAccuDBStatus {
    CYANRIP_ACCUDB_DISABLED = 0,
    CYANRIP_ACCUDB_NOT_FOUND,
    CYANRIP_ACCUDB_ERROR,
    CYANRIP_ACCUDB_MISMATCH,
    CYANRIP_ACCUDB_FOUND,
};

enum CRIPPathType {
    CRIP_PATH_COVERART, /* arg must be a CRIPArt * */
    CRIP_PATH_TRACK, /* arg must be a cyanrip_track * */
    CRIP_PATH_DATA, /* arg must be a cyanrip_track * */
    CRIP_PATH_LOG, /* arg must be NULL */
    CRIP_PATH_CUE, /* arg must be NULL */
};

enum CRIPSanitize {
    CRIP_SANITIZE_SIMPLE, /* Replace unacceptable symbols with _ */
    CRIP_SANITIZE_OS_SIMPLE, /* Same as above, but only replaces symbols not allowed on current OS */
    CRIP_SANITIZE_UNICODE, /* Replace unacceptable symbols with visually identical unicode equivalents */
    CRIP_SANITIZE_OS_UNICODE, /* Same as above, but only replaces symbols not allowed on current OS */
};

typedef struct cyanrip_settings {
    char *dev_path;
    char *folder_name_scheme;
    char *track_name_scheme;
    char *log_name_scheme;
    char *cue_name_scheme;
    enum CRIPSanitize sanitize_method;
    int speed;
    int max_retries;
    int offset;
    int over_under_read_frames;
    int print_info_only;
    int disable_mb;
    float bitrate;
    int decode_hdcd;
    int disable_accurip;
    int disable_coverart_db;
    int overread_leadinout;
    int eject_on_success_rip;
    enum cyanrip_pregap_action pregap_action[99];
    int rip_indices_count;
    int rip_indices[99];
    int paranoia_level;
    int deemphasis;
    int force_deemphasis;
    int ripping_retries;
    int disable_coverart_embedding;

    enum cyanrip_output_formats outputs[CYANRIP_FORMATS_NB];
    int outputs_num;
} cyanrip_settings;

typedef struct CRIPAccuDBEntry {
    int confidence;
    uint32_t checksum; /* We don't know which version it is */
    uint32_t checksum_450;
} CRIPAccuDBEntry;

typedef struct CRIPArt {
    AVDictionary *meta;
    char *source_url;
    char *title; /* Temporary, used during parsing only, copied to meta, do not free */

    AVPacket *pkt;
    AVCodecParameters *params;

    uint8_t *data;
    size_t size;
    char *extension;
} CRIPArt;

typedef struct cyanrip_track {
    int number; /* Human readable track number, may be 0 */
    int cd_track_number; /* Actual track on the CD, may be 0 */
    AVDictionary *meta; /* Disc's AVDictionary gets copied here */
    int total_repeats; /* How many times the track was re-ripped */

    int track_is_data;
    int preemphasis;
    int preemphasis_in_subcode;

    size_t nb_samples; /* Track duration in samples */

    int frames_before_disc_start;
    lsn_t frames; /* Actual number of frames to read, != samples */
    int frames_after_disc_end;

    lsn_t pregap_lsn;
    lsn_t start_lsn;
    lsn_t start_lsn_sig;
    lsn_t end_lsn;
    lsn_t end_lsn_sig;

    /* CUE sheet generator only */
    lsn_t dropped_pregap_start;
    lsn_t merged_pregap_end;

    ptrdiff_t partial_frame_byte_offs;

    CRIPArt art; /* One cover art, will not be saved */

    int computed_crcs;
    uint32_t eac_crc;
    uint32_t acurip_checksum_v1;
    uint32_t acurip_checksum_v1_450;
    uint32_t acurip_checksum_v2;
    int acurip_track_is_first;
    int acurip_track_is_last;

    enum CRIPAccuDBStatus ar_db_status;
    CRIPAccuDBEntry *ar_db_entries;
    int ar_db_nb_entries;
    int ar_db_max_confidence;

    struct cyanrip_track *pt;
    struct cyanrip_track *nt;
} cyanrip_track;

typedef struct cyanrip_ctx {
    cdrom_drive_t     *drive;
    cdrom_paranoia_t  *paranoia;
    CdIo_t            *cdio;
    FILE              *logfile[CYANRIP_FORMATS_NB];
    FILE              *cuefile[CYANRIP_FORMATS_NB];
    cyanrip_settings   settings;

    cyanrip_track tracks[198];
    int nb_tracks; /* Total number of output tracks */
    int nb_cd_tracks; /* Total tracks the CD signals */
    int disregard_cd_isrc; /* If one track doesn't have ISRC, universally the rest won't */

    char *mb_submission_url;

    /* Non-track bound cover art */
    CRIPArt cover_arts[32];
    int nb_cover_arts;

    /* Drive caps */
    cdio_drive_read_cap_t  rcap;
    cdio_drive_write_cap_t wcap;
    cdio_drive_misc_cap_t  mcap;

    /* Metadata */
    AVDictionary *meta;
    enum CRIPAccuDBStatus ar_db_status;

    /* Destination folder */
    const char *base_dst_folder;

    int success;
    int total_error_count;
    lsn_t start_lsn;
    lsn_t end_lsn;
    lsn_t duration_frames;

    /* ETA */
    CRSlidingWinCtx eta_ctx;
    lsn_t frames_read;
    lsn_t frames_to_read;
} cyanrip_ctx;

typedef struct cyanrip_out_fmt {
    const char *name;
    const char *folder_suffix;
    const char *ext;
    const char *lavf_name;
    int coverart_supported;
    int compression_level;
    int lossless;
    enum AVCodecID codec;
} cyanrip_out_fmt;

extern const cyanrip_out_fmt crip_fmt_info[];

char *crip_get_path(cyanrip_ctx *ctx, enum CRIPPathType type, int create_dirs,
                    const cyanrip_out_fmt *fmt, void *arg);

extern uint64_t paranoia_status[PARANOIA_CB_FINISHED + 1];
extern const int crip_max_paranoia_level;
