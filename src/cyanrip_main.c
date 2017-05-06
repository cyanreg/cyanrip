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
#include "cyanrip_log.h"

bool quit_now = 0;

int cyanrip_init_metareader(cyanrip_ctx *ctx)
{
    if (!(ctx->cdio = cdio_open(ctx->settings.dev_path, DRIVER_LINUX)))
        return 1;
    return 0;
}

void cyanrip_end_metareader(cyanrip_ctx *ctx)
{
    if (ctx->disc_mcn)
        free(ctx->disc_mcn);

    cdio_destroy(ctx->cdio);
}

void cyanrip_ctx_end(cyanrip_ctx **s)
{
    cyanrip_ctx *ctx;
    if (!s || !*s)
        return;
    ctx = *s;
    for (int i = 0; i < ctx->drive->tracks; i++)
        free(ctx->tracks[i].samples);
    if (ctx->cdio)
        cyanrip_end_metareader(ctx);
    if (ctx->paranoia)
        cdio_paranoia_free(ctx->paranoia);
    if (ctx->drive)
        cdio_cddap_close(ctx->drive);
    free(ctx->tracks);
    free(ctx);
    *s = NULL;
}

int cyanrip_ctx_init(cyanrip_ctx **s, cyanrip_settings *settings)
{
    int rval;
    char *error = NULL;

    cyanrip_ctx *ctx = calloc(1, sizeof(cyanrip_ctx));

    ctx->drive = cdio_cddap_identify(settings->dev_path, CDDA_MESSAGE_LOGIT, &error);
    if (!ctx->drive) {
        cyanrip_log(ctx, 0, "Unable to init cdio context");
        if (error)
            cyanrip_log(ctx, 0, " - \"%s\"\n", error);
        else
            cyanrip_log(ctx, 0, "!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    cdio_cddap_verbose_set(ctx->drive, CDDA_MESSAGE_PRINTIT, CDDA_MESSAGE_PRINTIT);

    cyanrip_log(ctx, 1, "Opening drive...\n");
    rval = cdio_cddap_open(ctx->drive);
    if (rval < 0) {
        cyanrip_log(ctx, 0, "Unable to open device!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    cdio_cddap_speed_set(ctx->drive, settings->speed);

    ctx->paranoia = cdio_paranoia_init(ctx->drive);
    if (!ctx->paranoia) {
        cyanrip_log(ctx, 0, "Unable to init paranoia!\n");
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
        cyanrip_log(ctx, 0, "Unable to get disc duration!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    if (cyanrip_init_metareader(ctx)) {
        cyanrip_log(ctx, 0, "Unable to init cdio context!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    uint32_t offset = settings->offset*4;
    cdio_paranoia_seek(ctx->paranoia, offset, SEEK_SET);

    ctx->tracks = calloc(cdda_tracks(ctx->drive) + 1, sizeof(cyanrip_track));

    ctx->last_frame = cdio_cddap_disc_lastsector(ctx->drive);

    ctx->settings = *settings;

    *s = ctx;
    return 0;
}

void cyanrip_read_disc_meta(cyanrip_ctx *ctx)
{
    ctx->disc_mcn = cdio_get_mcn(ctx->cdio);
    strcpy(ctx->disc_name, "Album_name");
}

void cyanrip_read_frame(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char *err = NULL;
    int retries = ctx->settings.frame_max_retries;

    int16_t *samples = NULL;

    if (quit_now) {
        cyanrip_log(ctx, 0, "Quitting now!\n");
        cyanrip_ctx_end(&ctx);
        exit(1);
    }

    if (!retries)
        samples = cdio_paranoia_read(ctx->paranoia, NULL);
    else
        samples = cdio_paranoia_read_limited(ctx->paranoia, NULL, retries);

    if ((err = cdio_cddap_errors(ctx->drive))) {
        cyanrip_log(ctx, 0, "%s\n", err);
        free(err);
        err = NULL;
    }

    if ((err = cdio_cddap_messages(ctx->drive))) {
        cyanrip_log(ctx, 0, "%s\n", err);
        free(err);
        err = NULL;
    }

    if (!samples) {
        cyanrip_log(ctx, 0, "Frame read failed!\n");
        return;
    }

    memcpy(t->samples + t->nb_samples, samples, CDIO_CD_FRAMESIZE_RAW);
    t->nb_samples += CDIO_CD_FRAMESIZE_RAW >> 1;
}

int cyanrip_read_track(cyanrip_ctx *ctx, int index)
{
    uint32_t frames = 0;
    cyanrip_track *t = &ctx->tracks[index];

    t->index = index;

    if (index < ctx->drive->tracks) {
        frames += cdda_track_lastsector (ctx->drive, t->index + 1);
        if (frames > ctx->last_frame) {
            cyanrip_log(ctx, 0, "Track last frame larger than last disc frame!\n");
            return 1;
        }
        frames -= cdda_track_firstsector(ctx->drive, t->index + 1);
    } else {
        cyanrip_log(ctx, 0, "Invalid track index = %i\n", index);
        return 1;
    }

    sprintf(t->name, "Track %02i", index + 1);

    t->preemphasis = cdio_get_track_preemphasis(ctx->cdio, t->index + 1);

    mmc_isrc_track_read_subchannel(ctx->cdio, t->index + 1, t->isrc);

    t->samples = malloc(frames*CDIO_CD_FRAMESIZE_RAW);
    t->nb_samples = 0;

    for (int i = 0; i < frames; i++) {
        cyanrip_read_frame(ctx, t);
        if (!(i % ctx->settings.report_rate))
            cyanrip_log(NULL, 0, "\r%s progress - %0.2f%%", t->name, ((double)i/frames)*100.0f);
    }
    cyanrip_log(NULL, 0, "\r%s ripped!\n", t->name);

    cyanrip_crc_track(ctx, t);

    for (int i = 0; i < ctx->settings.outputs_num; i++)
        cyanrip_encode_track(ctx, t, &ctx->settings.outputs[i]);

    cyanrip_log_track_end(ctx, t);

    return 0;
}

void on_quit_signal(int signo)
{
    if (quit_now) {
        cyanrip_log(NULL, 0, "Force quitting\n");
        exit(1);
    }
    cyanrip_log(NULL, 0, "Trying to quit\n");
    quit_now = 1;
}

int main(void)
{
    int ret;

    cyanrip_ctx *ctx;
    cyanrip_settings settings;

    if (signal(SIGINT, on_quit_signal) == SIG_ERR)
        cyanrip_log(ctx, 0, "Can't init signal handler!\n");

    settings.dev_path = "/dev/sr0";
    settings.verbose = 1;
    settings.speed = 0;
    settings.frame_max_retries = 0;
    settings.paranoia_mode = PARANOIA_MODE_FULL;
    settings.report_rate = 20;
    settings.offset = 0;

    /* Debug */
    settings.outputs[0] = (struct cyanrip_output_settings){ CYANRIP_FORMAT_FLAC, 0.0f };
    settings.outputs_num = 1;

    if ((ret = cyanrip_ctx_init(&ctx, &settings)))
        return ret;

    cyanrip_read_disc_meta(ctx);

    cyanrip_log_init(ctx);
    cyanrip_log_start_report(ctx);

    for (int i = 0; i < ctx->drive->tracks; i++)
        ret = cyanrip_read_track(ctx, i);

    cyanrip_log_finish_report(ctx);
    cyanrip_log_end(ctx);

    cyanrip_ctx_end(&ctx);

    return 0;
}
