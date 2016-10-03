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

#include <cdio/mmc.h>
#include <cdio/cdda.h>
#include <cdio/paranoia.h>

/* You seriously want to encode to more than 4 formats? Increase this. */
#define CYANRIP_MAX_OUTPUTS 4

enum cyanrip_output_formats {
    CYANRIP_FORMAT_FLAC = 0,
    CYANRIP_FORMAT_WAVPACK,
    CYANRIP_FORMAT_TTA,
    CYANRIP_FORMAT_ALAC,

    CYANRIP_FORMAT_OPUS,
    CYANRIP_FORMAT_VORBIS,
    CYANRIP_FORMAT_MP3,

    CYANRIP_FORMAT_WAV,
    CYANRIP_FORMAT_RAW, /* Raw s16le 44100Hz 2ch interleaved data */
};

typedef struct cyanrip_track {
    char name[16];
    char isrc[16];
    int preemphasis;
    uint32_t samples;
    uint32_t crc_v1;
    uint32_t crc_v2;
} cyanrip_track;

typedef struct cyanrip_settings {
    char *file;
    int speed;
    int paranoia_mode;
    int frame_max_retries;
    int report_rate;
    uint32_t offset;

    enum cyanrip_output_formats output_formats[CYANRIP_MAX_OUTPUTS];
    int output_compression_level[CYANRIP_MAX_OUTPUTS];  /* Lossless only */
    int output_bitrate[CYANRIP_MAX_OUTPUTS];            /* Lossy formats only */
    int outputs_number;                                 /* Total number */
} cyanrip_settings;

typedef struct cyanrip_ctx {
    cdrom_drive_t     *drive;
    cdrom_paranoia_t  *paranoia;
    cyanrip_settings   settings;
    cyanrip_track     *tracks;

    int cur_track;

    int16_t *samples;
    size_t samples_num;

    uint32_t cur_frame;

    uint32_t duration;   /* Sectors/frames */
    uint32_t last_frame; /* Sector/frame */
} cyanrip_ctx;


/* Exposed, just in case someone wants to use cyanrip as a library */

int  cyanrip_sectors_to_duration(uint32_t sectors, char *str);

/* Reads a single frame to the context */
void cyanrip_read_frame(cyanrip_ctx *ctx);

/* Read a track into the context, set index to 0 to rip the entire CD instead
 * of a single track. */
int  cyanrip_read_track(cyanrip_ctx *ctx, int index);

void cyanrip_ctx_flush(cyanrip_ctx *ctx);
int  cyanrip_ctx_alloc_frames(cyanrip_ctx *ctx, uint32_t frames);

int  cyanrip_ctx_init(cyanrip_ctx **s, cyanrip_settings *settings);
void cyanrip_ctx_end(cyanrip_ctx **s);












