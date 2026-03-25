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
#include <stddef.h>
#include <string.h>

#include <libavutil/rational.h>
#include <libavutil/mathematics.h>
#include <libavutil/dict.h>

/* Sliding window */
#define MAX_ROLLING_WIN_ENTRIES 1024 * 16
typedef struct CRSlidingWinCtx {
    struct CRSlidingWinEntry {
        int64_t num;
        int64_t pts;
        AVRational tb;
    } entries[MAX_ROLLING_WIN_ENTRIES];
    int num_entries;
} CRSlidingWinCtx;

int64_t cr_sliding_win(CRSlidingWinCtx *ctx, int64_t num, int64_t pts,
                       AVRational tb, int64_t len, int do_avg);

char *cr_ffmpeg_file_path(const char *path);

static inline const char *dict_get(AVDictionary *dict, const char *key)
{
    AVDictionaryEntry *e = av_dict_get(dict, key, NULL, 0);
    return e ? e->value : NULL;
}

static inline void cyanrip_frames_to_cue(uint32_t frames, char *str)
{
    if (!str)
        return;
    const uint32_t min  = frames / (75 * 60);
    const uint32_t sec  = (frames - (min * 75 * 60)) / 75;
    const uint32_t left = frames - (min * 75 * 60) - (sec * 75);
    snprintf(str, 16, "%02i:%02i:%02i", min, sec, left);
}

/**
 * Writes the duration, in MSF as IEC/MMC specify the string (min:sec.ff), to
 * the char*. Min can be larger than 59. Make sure that the char* is at least 13
 * bytes long. Frames is nice to avoid any potential rounding error.
 */
static inline void cyanrip_frames_to_duration(uint32_t frames, char *str)
{
    if (!str)
        return;
    // 75 frames per second
    const uint32_t min    = frames / (75 * 60);
    const uint32_t sec    = (frames / 75) % 60;
    const uint32_t remain = frames % 75;
    snprintf(str, 13, "%02i:%02i.%02i", min, sec, remain);
}

static inline int cmp_numbers(const void *a, const void *b)
{
    return *((int *)a) > *((int *)b);
}
