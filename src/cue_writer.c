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

#include "cyanrip_log.h"
#include "cue_writer.h"

int cyanrip_cue_init(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        char *cuefile = crip_get_path(ctx, CRIP_PATH_CUE, 1,
                                      &crip_fmt_info[ctx->settings.outputs[i]],
                                      NULL);

        ctx->cuefile[i] = av_fopen_utf8(cuefile, "w");

        if (!ctx->cuefile[i]) {
            cyanrip_log(ctx, 0, "Couldn't open path \"%s\" for writing: %s!\n"
                        "Invalid folder name? Try -D <folder>.\n",
                        cuefile, av_err2str(AVERROR(errno)));
            av_freep(&cuefile);
            return 1;
        }

        av_freep(&cuefile);
    }

    return 0;
}

#define CLOG(FORMAT, DICT, TAG)                                                \
    do {                                                                       \
        if (dict_get(DICT, TAG)) {                                             \
            for (int Z = 0; Z < ctx->settings.outputs_num; Z++)                \
                fprintf(ctx->cuefile[Z], FORMAT, dict_get(DICT, TAG));         \
        }                                                                      \
    } while (0)

void cyanrip_cue_start(cyanrip_ctx *ctx)
{
    CLOG("REM MUSICBRAINZ_ID \"%s\"\n", ctx->meta, "discid");
    CLOG("REM DISCID \"%s\"\n", ctx->meta, "cddb");

    const AVDictionaryEntry *d = NULL;
    while ((d = av_dict_get(ctx->meta, "", d, AV_DICT_IGNORE_SUFFIX))) {
        if (strcmp(d->key, "discid") && strcmp(d->key, "cddb") &&
            strcmp(d->key, "disc_mcn") && strcmp(d->key, "album_artist") &&
            strcmp(d->key, "album") && strcmp(d->key, "title")) {
            for (int Z = 0; Z < ctx->settings.outputs_num; Z++) {
                char *tmp = av_strdup(d->key);
                for (int i = 0; tmp[i]; i++)
                    tmp[i] = av_toupper(tmp[i]);
                fprintf(ctx->cuefile[Z], "REM %s \"%s\"\n", tmp, d->value);
                av_free(tmp);
            }
        }
    }

    CLOG("MCN \"%s\"\n", ctx->meta, "disc_mcn");
    CLOG("PERFORMER \"%s\"\n", ctx->meta, "album_artist");
    CLOG("TITLE \"%s\"\n", ctx->meta, "album");
}

void cyanrip_cue_track(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char time_00[16];
    char time_01[16];

    /* Finish over the pregap which has been appended to the last track */
    if (t->pregap_lsn != CDIO_INVALID_LSN && t->pt &&
        t->dropped_pregap_start == CDIO_INVALID_LSN &&
        t->merged_pregap_end == CDIO_INVALID_LSN) {
        for (int Z = 0; Z < ctx->settings.outputs_num; Z++)
            fprintf(ctx->cuefile[Z], "  TRACK %02d AUDIO\n", t->index);

        CLOG("    TITLE \"%s\"\n", t->meta, "title");
        CLOG("    PERFORMER \"%s\"\n", t->meta, "artist");

        cyanrip_frames_to_cue(t->pregap_lsn - t->pt->start_lsn, time_00);
        for (int Z = 0; Z < ctx->settings.outputs_num; Z++)
            fprintf(ctx->cuefile[Z], "    INDEX 00 %s\n", time_00);
    }

    for (int Z = 0; Z < ctx->settings.outputs_num; Z++) {
        char *path = crip_get_path(ctx,
                                   t->track_is_data ? CRIP_PATH_DATA : CRIP_PATH_TRACK,
                                   0, &crip_fmt_info[ctx->settings.outputs[Z]],
                                   t);
        char *name = path;
        for (int i = 0; name[i]; i++) {
            if (name[i] == '/') {
                name = &name[i + 1];
                break;
            }
        }

        fprintf(ctx->cuefile[Z], "FILE \"%s\" %s\n", name,
                ctx->settings.outputs[Z] == CYANRIP_FORMAT_MP3 ? "MP3" :
                t->track_is_data ? "BINARY" : "WAVE");

        fprintf(ctx->cuefile[Z], "  TRACK %02d %s\n", t->number,
                t->track_is_data ? "MODE1/2352" : "AUDIO");

        av_free(path);
    }

    if (!t->track_is_data) {
        CLOG("    TITLE \"%s\"\n", t->meta, "title");
        CLOG("    PERFORMER \"%s\"\n", t->meta, "artist");
        CLOG("    ISRC %s\n", t->meta, "isrc");
    }

    if (t->dropped_pregap_start != CDIO_INVALID_LSN) {
        cyanrip_frames_to_cue(t->start_lsn - t->dropped_pregap_start, time_00);
        cyanrip_frames_to_cue(0, time_01);
    } else if (t->merged_pregap_end != CDIO_INVALID_LSN) {
        cyanrip_frames_to_cue(0, time_00);
        cyanrip_frames_to_cue(t->merged_pregap_end - t->start_lsn, time_01);
    } else {
        cyanrip_frames_to_cue(0, time_01);
    }

    for (int Z = 0; Z < ctx->settings.outputs_num; Z++) {
        if (t->preemphasis && !ctx->settings.deemphasis &&
            !ctx->settings.force_deemphasis)
            fprintf(ctx->cuefile[Z], "    FLAGS PRE\n");

        if (t->dropped_pregap_start != CDIO_INVALID_LSN) {
            fprintf(ctx->cuefile[Z], "    PREGAP %s\n",   time_00);
            fprintf(ctx->cuefile[Z], "    INDEX 01 %s\n", time_01);
        } else if (t->merged_pregap_end != CDIO_INVALID_LSN) {
            fprintf(ctx->cuefile[Z], "    INDEX 00 %s\n", time_00);
            fprintf(ctx->cuefile[Z], "    INDEX 01 %s\n", time_01);
        } else {
            fprintf(ctx->cuefile[Z], "    INDEX 01 %s\n", time_01);
        }
    }
}

void cyanrip_cue_end(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        if (!ctx->cuefile[i])
            continue;

        fclose(ctx->cuefile[i]);
        ctx->cuefile[i] = NULL;
    }
}
