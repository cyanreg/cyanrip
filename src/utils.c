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

#include "utils.h"
#include <libavutil/avstring.h>
#include <libavutil/mem.h>

int64_t cr_sliding_win(CRSlidingWinCtx *ctx, int64_t num, int64_t pts,
                       AVRational tb, int64_t len, int do_avg)
{
    struct CRSlidingWinEntry *top, *last;
    int64_t sum = 0;

    if (pts == INT64_MIN)
        goto calc;

    if (!ctx->num_entries)
        goto add;

    for (int i = 0; i < ctx->num_entries; i++) {
        last = &ctx->entries[i];
        int64_t test = av_add_stable(last->tb, last->pts, tb, len);

        if (((ctx->num_entries + 1) > MAX_ROLLING_WIN_ENTRIES) ||
            av_compare_ts(test, last->tb, pts, tb) < 0) {
            ctx->num_entries--;
            memmove(last, last + 1, sizeof(*last) * ctx->num_entries);
        }
    }

add:
    top = &ctx->entries[ctx->num_entries++];
    top->num = num;
    top->pts = pts;
    top->tb  = tb;

calc:
    /* Calculate the average */
    for (int i = 0; i < ctx->num_entries; i++)
        sum += ctx->entries[i].num;

    if (do_avg && ctx->num_entries)
        sum /= ctx->num_entries;

    return sum;
}

char *cr_ffmpeg_file_path(const char *path)
{
    const char *file_proto = "file:";
    const int len = strlen(path) + strlen(file_proto) + 1;
    char *rep_str = av_mallocz(len);
    if (!rep_str)
        return NULL;

    av_strlcpy(rep_str, file_proto, len);
    av_strlcat(rep_str, path, len);
    return rep_str;
}
