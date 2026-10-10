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

#include <libavutil/bprint.h>

#include "cyanrip_log.h"
#include "cue_writer.h"
#include "os_compat.h"

int cyanrip_cue_init(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        char *cuefile = crip_get_path(ctx, CRIP_PATH_CUE, 1,
                                      &crip_fmt_info[ctx->settings.outputs[i]],
                                      NULL);

        ctx->cuefile[i] = fopen(cuefile, "wb+");

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

/* CUE sheets have no way of escaping a quote inside a quoted string, so
 * replace them the way file names get them, and keep values on one line */
static char *cue_str(cyanrip_ctx *ctx, const char *str)
{
    int unicode = ctx->settings.sanitize_method == CRIP_SANITIZE_UNICODE ||
                  ctx->settings.sanitize_method == CRIP_SANITIZE_OS_UNICODE;
    int quote = 0;
    char *ret = NULL;
    AVBPrint bp;

    if (!str)
        return NULL;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    for (; *str; str++) {
        if (*str == '"') {
            av_bprintf(&bp, "%s", !unicode ? "'" : quote ? "”" : "“");
            quote ^= 1;
        } else if (*str == '\n' || *str == '\r') {
            av_bprint_chars(&bp, ' ', 1);
        } else {
            av_bprint_chars(&bp, *str, 1);
        }
    }

    av_bprint_finalize(&bp, &ret);
    return ret;
}

#define CLOG(FORMAT, DICT, TAG)                                                \
    do {                                                                       \
        char *val = cue_str(ctx, dict_get(DICT, TAG));                         \
        if (val) {                                                             \
            for (int Z = 0; Z < ctx->settings.outputs_num; Z++)                \
                fprintf(ctx->cuefile[Z], FORMAT, val);                         \
        }                                                                      \
        av_free(val);                                                          \
    } while (0)

void cyanrip_cue_start(cyanrip_ctx *ctx)
{
    CLOG("REM MUSICBRAINZ_ID \"%s\"\n", ctx->meta, "musicbrainz_discid");
    CLOG("REM DISCID \"%s\"\n", ctx->meta, "cddb");

    const AVDictionaryEntry *d = NULL;
    while ((d = av_dict_get(ctx->meta, "", d, AV_DICT_IGNORE_SUFFIX))) {
        if (strcmp(d->key, "musicbrainz_discid") && strcmp(d->key, "cddb") &&
            strcmp(d->key, "disc_mcn") && strcmp(d->key, "album_artist") &&
            strcmp(d->key, "album") && strcmp(d->key, "title")) {
            for (int Z = 0; Z < ctx->settings.outputs_num; Z++) {
                char *tmp = av_strdup(d->key);
                char *val = cue_str(ctx, d->value);
                for (int i = 0; tmp[i]; i++)
                    tmp[i] = av_toupper(tmp[i]);
                fprintf(ctx->cuefile[Z], "REM %s \"%s\"\n", tmp, val);
                av_free(val);
                av_free(tmp);
            }
        }
    }

    CLOG("CATALOG %s\n", ctx->meta, "disc_mcn");
    CLOG("PERFORMER \"%s\"\n", ctx->meta, "album_artist");
    CLOG("TITLE \"%s\"\n", ctx->meta, "album");
}

void cyanrip_cue_track(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char time_00[16];
    char time_01[16];

    /* Finish over the pregap which has been appended to the last track */
    const int write_appended_pregap = (
        t->pregap_lsn != CDIO_INVALID_LSN && t->pregap_lsn != t->start_lsn && t->pt
        && t->dropped_pregap_start == CDIO_INVALID_LSN
        && t->merged_pregap_end == CDIO_INVALID_LSN);
    if (write_appended_pregap) {
        for (int Z = 0; Z < ctx->settings.outputs_num; Z++)
            fprintf(ctx->cuefile[Z], "  TRACK %02d AUDIO\n", t->number);

        CLOG("    TITLE \"%s\"\n", t->meta, "title");
        CLOG("    PERFORMER \"%s\"\n", t->meta, "artist");

        cyanrip_frames_to_cue(t->pregap_lsn - t->pt->start_lsn_sig, time_00);
        for (int Z = 0; Z < ctx->settings.outputs_num; Z++)
            fprintf(ctx->cuefile[Z], "    INDEX 00 %s\n", time_00);
    }

    /* A hidden track split off track 1 can't be track 0, which CUE sheets
     * don't have, so its file holds track 1's INDEX 00 instead, and track
     * 1's own file only its INDEX 01 */
    const int htoa = t->number == 0 && t->nt && !t->track_is_data;
    const int after_htoa = t->pt && t->pt->number == 0 && !t->pt->track_is_data;
    const cyanrip_track *tm = htoa ? t->nt : t; /* Whose TRACK this file starts */

    for (int Z = 0; Z < ctx->settings.outputs_num; Z++) {
        char *path = crip_get_path(ctx,
                                   t->track_is_data ? CRIP_PATH_DATA : CRIP_PATH_TRACK,
                                   0, &crip_fmt_info[ctx->settings.outputs[Z]],
                                   t);
        /* Reference the file relative to the CUE sheet's own directory */
        char *name = path;
        char *cuepath = crip_get_path(ctx, CRIP_PATH_CUE, 0,
                                      &crip_fmt_info[ctx->settings.outputs[Z]],
                                      NULL);
        if (cuepath) {
            char *sep = strrchr(cuepath, OS_DIR_CHAR);
            size_t plen = sep ? (sep - cuepath) + 1 : 0;
            if (plen && !strncmp(path, cuepath, plen))
                name = path + plen;
            av_free(cuepath);
        }

        fprintf(ctx->cuefile[Z], "FILE \"%s\" %s\n", name,
                ctx->settings.outputs[Z] == CYANRIP_FORMAT_MP3 ? "MP3" :
                t->track_is_data ? "BINARY" : "WAVE");

        if (!write_appended_pregap && !after_htoa)
            fprintf(ctx->cuefile[Z], "  TRACK %02d %s\n", tm->number,
                    !t->track_is_data ? "AUDIO" :
                    t->data_mode == 2 ? "MODE2/2352" : "MODE1/2352");

        av_free(path);
    }

    if (!t->track_is_data && !write_appended_pregap && !after_htoa) {
        CLOG("    TITLE \"%s\"\n", tm->meta, "title");
        CLOG("    PERFORMER \"%s\"\n", tm->meta, "artist");
        CLOG("    ISRC %s\n", tm->meta, "isrc");
    }

    if (t->dropped_pregap_start != CDIO_INVALID_LSN) {
        cyanrip_frames_to_cue(t->start_lsn_sig - t->dropped_pregap_start, time_00);
        cyanrip_frames_to_cue(0, time_01);
    } else if (t->merged_pregap_end != CDIO_INVALID_LSN) {
        cyanrip_frames_to_cue(0, time_00);
        cyanrip_frames_to_cue(t->merged_pregap_end - t->start_lsn_sig, time_01);
    } else {
        cyanrip_frames_to_cue(0, time_01);
    }

    for (int Z = 0; Z < ctx->settings.outputs_num; Z++) {
        if (!after_htoa && tm->preemphasis && !ctx->settings.deemphasis &&
            !ctx->settings.force_deemphasis)
            fprintf(ctx->cuefile[Z], "    FLAGS PRE\n");

        if (htoa) {
            fprintf(ctx->cuefile[Z], "    INDEX 00 %s\n", time_01);
        } else if (t->dropped_pregap_start != CDIO_INVALID_LSN &&
            t->dropped_pregap_start != t->start_lsn) {
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
