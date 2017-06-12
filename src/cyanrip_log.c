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

#include <stdarg.h>
#include <time.h>

#include "cyanrip_encode.h"
#include "cyanrip_log.h"

void cyanrip_log_track_end(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char length[16];
    cyanrip_samples_to_duration(t->nb_samples >> 1, length);

    cyanrip_log(ctx, 0, "Track %i completed successfully!\n", t->index + 1);
    cyanrip_log(ctx, 0, "    Title:       %s\n", t->name);
    cyanrip_log(ctx, 0, "    ISRC:        %s\n", t->isrc);
    cyanrip_log(ctx, 0, "    Preemphasis: %i\n", t->preemphasis);
    cyanrip_log(ctx, 0, "    Duration:    %s\n", length);
    cyanrip_log(ctx, 0, "    Samples:     %u\n", t->nb_samples);
    cyanrip_log(ctx, 0, "    IEEE CRC 32: 0x%08x\n", t->ieee_crc_32);
    cyanrip_log(ctx, 0, "\n");
}

void cyanrip_log_start_report(cyanrip_ctx *ctx)
{
    cyanrip_log(ctx, 0, "%s\n",                PROGRAM_STRING);
    cyanrip_log(ctx, 0, "Device:        %s\n", ctx->drive->drive_model);
    cyanrip_log(ctx, 0, "Offset:        %c%i samples\n", ctx->settings.offset >= 0 ? '+' : '-', abs(ctx->settings.offset));
    cyanrip_log(ctx, 0, "Path:          %s\n", ctx->settings.dev_path);
    if (ctx->settings.cover_image_path)
        cyanrip_log(ctx, 0, "Album Art:     %s\n", ctx->settings.cover_image_path);
    cyanrip_log(ctx, 0, "Base folder:   %s\n", ctx->settings.base_dst_folder ?
                                               ctx->settings.base_dst_folder :
                                               strlen(ctx->disc_name) ? ctx->disc_name : ctx->discid);
    if (ctx->settings.speed)
        cyanrip_log(ctx, 0, "Speed:         %ix\n", ctx->settings.speed);
    else
        cyanrip_log(ctx, 0, "Speed:         default\n");
    cyanrip_log(ctx, 0, "Paranoia:      %s\n",
                ctx->settings.paranoia_mode == PARANOIA_MODE_DISABLE ? "fast" : "full");
    cyanrip_log(ctx, 0, "Retries:       %i\n", ctx->settings.frame_max_retries);
    cyanrip_log(ctx, 0, "Outputs:       ");
    for (int i = 0; i < ctx->settings.outputs_num; i++)
        cyanrip_log(ctx, 0, "%s%s", cyanrip_fmt_desc(ctx->settings.outputs[i]), i != (ctx->settings.outputs_num - 1) ? ", " : "");
    cyanrip_log(ctx, 0, " (lossy bitrate: %.02fkbps)\n", ctx->settings.bitrate);
    cyanrip_log(ctx, 0, "Disc tracks:   %i\n", ctx->drive->tracks);
    cyanrip_log(ctx, 0, "Tracks to rip: %s", (ctx->settings.rip_indices_count == -1) ? "all" : !ctx->settings.rip_indices_count ? "none" : "");
    if (ctx->settings.rip_indices_count != -1) {
        for (int i = 0; i < ctx->settings.rip_indices_count; i++)
            cyanrip_log(ctx, 0, "%i%s", ctx->settings.rip_indices[i], i != (ctx->settings.rip_indices_count - 1) ? ", " : "");
    }
    cyanrip_log(ctx, 0, "\n");

    char duration[16];
    cyanrip_frames_to_duration(ctx->duration, duration);
    cyanrip_log(ctx, 0, "DiscID:        %s\n", ctx->discid);
    cyanrip_log(ctx, 0, "Disc MCN:      %s\n", ctx->disc_mcn);
    cyanrip_log(ctx, 0, "Album:         %s\n", ctx->disc_name);
    cyanrip_log(ctx, 0, "Artist:        %s\n", ctx->album_artist);
    cyanrip_log(ctx, 0, "Total time:    %s\n", duration);

    cyanrip_log(ctx, 0, "\n\n");
}

void cyanrip_log_finish_report(cyanrip_ctx *ctx)
{
    char t_s[64];
    time_t t_c = time(NULL);
    struct tm *t_l = localtime(&t_c);
    strftime(t_s, sizeof(t_s), "%Y-%m-%dT%H:%M:%S", t_l);

    cyanrip_log(ctx, 0, "Ripping errors: %i\n", ctx->errors_count);
    cyanrip_log(ctx, 0, "Ripping finished at %s\n", t_s);
}

int cyanrip_log_init(cyanrip_ctx *ctx)
{
    char logfile[260];
    if (strlen(ctx->disc_name))
        sprintf(logfile, "%s.%s", ctx->disc_name, "log");
    else
        sprintf(logfile, "%s.%s", ctx->discid, "log");
    ctx->logfile = fopen(logfile, "w");
    if (!ctx->logfile) {
        cyanrip_log(ctx, 0, "Error opening log file to write to!\n");
        return 1;
    }
    return 0;
}

void cyanrip_log_end(cyanrip_ctx *ctx)
{
    if (!ctx->logfile)
        return;
    fclose(ctx->logfile);
}

void cyanrip_log(cyanrip_ctx *ctx, int verbose, const char *format, ...)
{
    va_list args;
    if (ctx && ctx->logfile) {
        va_start(args, format);
        vfprintf(ctx->logfile, format, args);
        va_end(args);
    }
    if (ctx && !ctx->settings.verbose && !verbose)
        return;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
