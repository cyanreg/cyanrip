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

#include "cyanrip_main.h"
#include "cyanrip_log.h"
#include "cyanrip_crc.h"
#include "cyanrip_encode.h"

int cyanrip_frames_to_duration(uint32_t sectors, char *str)
{
    if (!str)
        return 1;
    const double tot = sectors/75.0f; /* 75 frames per second */
    const int hr    = tot/3600.0f;
    const int min   = (tot/60.0f) - (hr * 60);
    const int sec   = tot - ((hr * 3600) + min * 60);
    const int msec  = tot - sec;
    snprintf(str, 12, "%02i:%02i:%02i.%i", hr, min, sec, msec);
    return 0;
}

void cyanrip_ctx_flush(cyanrip_ctx *ctx)
{
    if (!ctx)
        return;
    free(ctx->samples);
    ctx->samples = 0;
    ctx->samples_num = 0;
}

int cyanrip_ctx_alloc_frames(cyanrip_ctx *ctx, uint32_t frames)
{
    if (!ctx)
        return 1;
    cyanrip_ctx_flush(ctx);
    ctx->samples = malloc(frames*CDIO_CD_FRAMESIZE_RAW);
    if (!ctx->samples) {
        fprintf(stderr, "Unable to allocate memory!\n");
        return 1;
    }
    return 0;
}

void cyanrip_ctx_end(cyanrip_ctx **s)
{
    cyanrip_ctx *ctx;
    if (!s || !*s)
        return;
    ctx = *s;
    if (ctx->paranoia)
        cdio_paranoia_free(ctx->paranoia);
    if (ctx->drive)
        cdio_cddap_close(ctx->drive);
    free(ctx);
    *s = NULL;
}

int cyanrip_ctx_init(cyanrip_ctx **s, cyanrip_settings *settings)
{
    int rval;
    char *error = NULL;

    cyanrip_ctx *ctx = calloc(1, sizeof(cyanrip_ctx));

    ctx->drive = cdio_cddap_identify(settings->file, CDDA_MESSAGE_LOGIT, &error);
    if (!ctx->drive) {
        fprintf(stderr, "Unable to init cdio context");
        if (error)
            fprintf(stderr, " - \"%s\"\n", error);
        else
            fprintf(stderr, "!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    rval = cdio_cddap_open(ctx->drive);
    if (rval < 0) {
        fprintf(stderr, "Unable to open device!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    cdio_cddap_verbose_set(ctx->drive, CDDA_MESSAGE_LOGIT, CDDA_MESSAGE_LOGIT);

    cdio_cddap_speed_set(ctx->drive, settings->speed);

    ctx->paranoia = cdio_paranoia_init(ctx->drive);
    if (!ctx->paranoia) {
        fprintf(stderr, "Unable to init paranoia!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    cdio_paranoia_modeset(ctx->paranoia, settings->paranoia_mode);

    if (ctx->drive->audio_last_sector  != CDIO_INVALID_LSN &&
        ctx->drive->audio_first_sector != CDIO_INVALID_LSN) {
        ctx->duration = ctx->drive->audio_last_sector - ctx->drive->audio_first_sector;
    } else if (ctx->drive->tracks) {
        ctx->duration = cdda_track_lastsector(ctx->drive, ctx->drive->tracks);
    } else {
        fprintf(stderr, "Unable to get disc duration!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    uint32_t offset = settings->offset*4;
    cdio_paranoia_seek(ctx->paranoia, offset, SEEK_SET);

    ctx->tracks = calloc(cdda_tracks(ctx->drive) + 1, sizeof(cyanrip_track));

    ctx->last_frame = cdio_cddap_disc_lastsector(ctx->drive);
    ctx->cur_frame = 0;

    ctx->settings = *settings;

    *s = ctx;
    return 0;
}

int cyanrip_get_text_isrc(cyanrip_ctx *ctx)
{
    CdIo_t *cdio = cdio_open(ctx->settings.file, DRIVER_LINUX);

    if (!cdio)
        return 1;

    for (int i = 1; i < ctx->drive->tracks + 1; i++) {
        mmc_isrc_track_read_subchannel(cdio, 1, ctx->tracks[i].isrc);
        ctx->tracks[i].preemphasis = cdio_get_track_preemphasis(cdio, i);
    }

    cdio_destroy(cdio);

    return 0;
}

void cyanrip_read_frame(cyanrip_ctx *ctx)
{
    char *err = NULL;
    int retries = ctx->settings.frame_max_retries;

    int16_t *samples = NULL;
    const int num = CDIO_CD_FRAMESIZE_RAW >> 1;

    if (!retries)
        samples = cdio_paranoia_read(ctx->paranoia, NULL);
    else
        samples = cdio_paranoia_read_limited(ctx->paranoia, NULL, retries);

    if ((err = cdio_cddap_errors(ctx->drive))) {
        fprintf(stderr, "%s\n", err);
        free(err);
        err = NULL;
    }

    if ((err = cdio_cddap_messages(ctx->drive))) {
        fprintf(stderr, "%s\n", err);
        free(err);
        err = NULL;
    }

    if (!samples) {
        fprintf(stderr, "Frame read failed!\n");
        return;
    }

    memcpy(ctx->samples + ctx->samples_num, samples, CDIO_CD_FRAMESIZE_RAW);
    ctx->samples_num += num;
    ctx->cur_frame++;
}

int cyanrip_read_track(cyanrip_ctx *ctx, int index)
{
    uint32_t frames = 0;

    if (index > 0 && (index < ctx->drive->tracks + 1)) {
        frames += cdda_track_lastsector (ctx->drive, index);
        frames -= cdda_track_firstsector(ctx->drive, index);
    } else if (index == 0) {
        frames = ctx->duration;
    } else {
        fprintf(stderr, "Invalid track index = %i\n", index);
        return 1;
    }

    sprintf(ctx->tracks[index].name, "track %i", index);

    ctx->cur_track = index;

    cyanrip_ctx_alloc_frames(ctx, frames);

    for (int i = 0; i < frames; i++) {
        cyanrip_read_frame(ctx);
        if (!(i % ctx->settings.report_rate))
            fprintf(stderr, "\rProgress - %0.2f%%", ((double)i/frames)*100.0f);
    }
    fprintf(stderr, "\r");

    cyanrip_crc_track(ctx);

    cyanrip_encode_track(ctx);

    return 0;
}

int main(void)
{
    int ret;

    cyanrip_ctx *ctx;
    cyanrip_settings settings;

    settings.file = "/dev/sr0";
    settings.speed = 0;
    settings.frame_max_retries = 0;
    settings.paranoia_mode = PARANOIA_MODE_FULL;
    settings.report_rate = 100;
    settings.offset = 0;

    settings.output_formats[0] = CYANRIP_FORMAT_FLAC;
    settings.output_compression_level[0] = 10;

    settings.output_formats[1] = CYANRIP_FORMAT_MP3;
    settings.output_bitrate[1] = 192;

    settings.outputs_number = 2;

    /* Debug */
    settings.output_formats[0] = CYANRIP_FORMAT_WAV;
    settings.outputs_number = 1;

    if ((ret = cyanrip_ctx_init(&ctx, &settings)))
        return ret;

    fprintf(stderr, "Device: %s\n", ctx->drive->drive_model);

    char duration[16];
    cyanrip_frames_to_duration(ctx->duration, duration);
    fprintf(stderr, "Total time = %s\n", duration);

    //cyanrip_get_text_isrc(ctx);

    for (int i = 1; i < ctx->drive->tracks + 1; i++) {
        ret = cyanrip_read_track(ctx, i);
        if (!ret)
            fprintf(stderr, "Track %i (%s) successfully ripped! (%x   %x)\n", i, ctx->tracks[i].isrc, ctx->tracks[i].crc_v1, ctx->tracks[i].crc_v2);
    }

    cyanrip_create_log(ctx);
    cyanrip_create_cue(ctx);

    cyanrip_ctx_end(&ctx);

    return 0;
}
