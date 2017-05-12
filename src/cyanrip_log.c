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

#include "cyanrip_log.h"

void cyanrip_log_track_end(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char length[16];
    cyanrip_samples_to_duration(t->nb_samples, length);

    cyanrip_log(ctx, 0, "Track %s completed successfully!\n", t->name);
    cyanrip_log(ctx, 0, "    ISRC:        %s\n", t->isrc);
    cyanrip_log(ctx, 0, "    Preemphasis: %i\n", t->preemphasis);
    cyanrip_log(ctx, 0, "    Duration:    %s\n", length);
    cyanrip_log(ctx, 0, "    Samples:     %u\n", t->nb_samples);
    cyanrip_log(ctx, 0, "    IEEE CRC 32: 0x%08x\n", t->ieee_crc_32);
    cyanrip_log(ctx, 0, "    Accurip v1:  0x%08x\n", t->acurip_crc_v1);
    cyanrip_log(ctx, 0, "    Accurip v2:  0x%08x\n", t->acurip_crc_v2);
    cyanrip_log(ctx, 0, "\n");
}

void cyanrip_log_start_report(cyanrip_ctx *ctx)
{
    cyanrip_log(ctx, 0, "Device:     %s\n", ctx->drive->drive_model);

    char duration[16];
    cyanrip_frames_to_duration(ctx->duration, duration);
    cyanrip_log(ctx, 0, "DiscID:     %s\n", ctx->discid);
    cyanrip_log(ctx, 0, "Disc MCN:   %s\n", ctx->disc_mcn);
    cyanrip_log(ctx, 0, "Total time: %s\n", duration);

    cyanrip_log(ctx, 0, "\n\n");
}

void cyanrip_log_finish_report(cyanrip_ctx *ctx)
{

}

int cyanrip_log_init(cyanrip_ctx *ctx)
{
    char logfile[260];
    sprintf(logfile, "%s.%s", ctx->disc_name, "log");
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
