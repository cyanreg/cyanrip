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
#include "config.h"

#ifdef HAVE_CDIO_PARANOIA_PARANOIA_H
#include <cdio/paranoia/paranoia.h>
#else
#include <cdio/paranoia.h>
#endif

#include <discid/discid.h>
#include <musicbrainz5/mb5_c.h>

enum cyanrip_output_formats {
    CYANRIP_FORMAT_FLAC,
    CYANRIP_FORMAT_TTA,
    CYANRIP_FORMAT_OPUS,
    CYANRIP_FORMAT_AAC,
    CYANRIP_FORMAT_WAVPACK,
    CYANRIP_FORMAT_ALAC,
    CYANRIP_FORMAT_MP3,
    CYANRIP_FORMAT_VORBIS,

    CYANRIP_FORMATS_NB,
};

typedef struct cyanrip_settings {
    char *dev_path;
    char *cover_image_path;
    char *base_dst_folder;
    bool verbose;
    int speed;
    int fast_mode;
    int frame_max_retries;
    int report_rate;
    int offset;
    int over_under_read_frames;
    int disable_mb;
    int eject_after;
    float bitrate;
    int rip_indices_count;
    int rip_indices[99];

    enum cyanrip_output_formats outputs[CYANRIP_FORMATS_NB];
    int outputs_num;
} cyanrip_settings;

typedef struct cyanrip_track {
    /* Metadata */
    int index;
    char name[256];
    char artist[256];
    char *isrc;
    int preemphasis;
    size_t nb_samples;
    uint32_t ieee_crc_32;
    uint32_t eac_crc;
    int start_sector;
    int end_sector;
    uint32_t acurip_crc_v1;
    uint32_t acurip_crc_v2;
    /* Metadata */

    int16_t *samples;       /* Actual compensated track data with length nb_samples */
    uint8_t *base_data;   /* Data without CD drive offset or underread compensation */
} cyanrip_track;

typedef struct cyanrip_ctx {
    cdrom_drive_t     *drive;
    cdrom_paranoia_t  *paranoia;
    cyanrip_settings   settings;
    cyanrip_track     *tracks;
    CdIo_t            *cdio;
    DiscId            *discid_ctx;
    FILE              *logfile;

    /* Metadata */
    char album_artist[256];
    char disc_name[256];
    char *disc_mcn;
    struct tm *disc_date;
    char discid[64];
    /* Metadata */

    void *cover_image_pkt; /* Cover image, init using cyanrip_setup_cover_image() */
    void *cover_image_params;

    int errors_count;
    lsn_t duration;
    lsn_t last_frame;
} cyanrip_ctx;

static inline void cyanrip_frames_to_duration(uint32_t sectors, char *str)
{
    if (!str)
        return;
    const double tot = sectors/75.0f; /* 75 frames per second */
    const int hr    = tot/3600.0f;
    const int min   = (tot/60.0f) - (hr * 60);
    const int sec   = tot - ((hr * 3600) + min * 60);
    const int msec  = tot - sec;
    snprintf(str, 12, "%02i:%02i:%02i.%i", hr, min, sec, msec);
}

static inline void cyanrip_samples_to_duration(uint32_t samples, char *str)
{
    if (!str)
        return;
    const double tot = samples/44100.0; /* 44100 samples per second */
    const int hr    = tot/3600.0f;
    const int min   = (tot/60.0f) - (hr * 60);
    const int sec   = tot - ((hr * 3600) + min * 60);
    const int msec  = tot - sec;
    snprintf(str, 12, "%02i:%02i:%02i.%i", hr, min, sec, msec);
}

static inline char *cyanrip_sanitize_fn(char *str)
{
    char forbiddenchars[] = "<>:/\\|?*";
    char *ret = str;
    while(*str) {
        if (*str == '"')
            *str = '\'';
        else if (strchr(forbiddenchars, *str))
            *str = '_';
        str++;
    }
    return ret;
}
