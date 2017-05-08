/*
 * Copyright (C) 2016 Rostislav Pehlivanov <atomnuker@gmail.com>
 *
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

#include <cdio/cdda.h>
#include <cdio/paranoia.h>
#include <cdio/mmc.h>

enum cyanrip_cover_image_formats {
    CYANRIP_COVER_IMAGE_JPG,
    CYANRIP_COVER_IMAGE_PNG,
};

enum cyanrip_output_formats {
    CYANRIP_FORMAT_FLAC,
    CYANRIP_FORMAT_TTA,
};

typedef struct cyanrip_output_settings {
    enum cyanrip_output_formats format;
    float bitrate; /* Lossy formats only in kbps */
} cyanrip_output_settings;

typedef struct cyanrip_settings {
    char *dev_path;
    char *cover_image_path;
    bool verbose;
    int speed;
    int paranoia_mode;
    int frame_max_retries;
    int report_rate;
    uint32_t offset;

    cyanrip_output_settings outputs[10];
    int outputs_num;
} cyanrip_settings;

typedef struct cyanrip_track {
    /* Metadata */
    int index;
    char name[256];
    char isrc[16];
    int preemphasis;
    size_t nb_samples;
    uint32_t ieee_crc_32;
    uint32_t acurip_crc_v1;
    uint32_t acurip_crc_v2;
    /* Metadata */

    int16_t *samples;
} cyanrip_track;

typedef struct cyanrip_ctx {
    cdrom_drive_t     *drive;
    cdrom_paranoia_t  *paranoia;
    cyanrip_settings   settings;
    cyanrip_track     *tracks;
    CdIo_t            *cdio;
    FILE              *logfile;

    /* Metadata */
    char album_artist[256];
    char disc_name[256];
    char *disc_mcn;
    struct tm *disc_date;
    /* Metadata */

    void *cover_image_pkt; /* Cover image, init using cyanrip_setup_cover_image() */
    int cover_image_codec_id;

    uint32_t duration;
    uint32_t last_frame;
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
