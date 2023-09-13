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
#include <pthread.h>

#include <libavutil/avutil.h>
#include <libavutil/sha512.h>
#include <libavutil/base64.h>

#include "cyanrip_encode.h"
#include "cyanrip_log.h"
#include "accurip.h"

#define CLOG(FORMAT, DICT, TAG)                                                \
    if (dict_get(DICT, TAG))                                                   \
        cyanrip_log(ctx, 0, FORMAT, dict_get(DICT, TAG));                      \

static void print_offsets(cyanrip_ctx *ctx, cyanrip_track *t)
{
    if (t->pregap_lsn != CDIO_INVALID_LSN) {
        char pregap_duration[16];
        cyanrip_frames_to_duration(t->start_lsn - t->pregap_lsn, pregap_duration);

        cyanrip_log(ctx, 0, "    Pregap LSN:  %i (duration: %s)\n",
                    t->pregap_lsn, pregap_duration);
    } else {
        cyanrip_log(ctx, 0, "    Pregap LSN:  none\n");
    }

    if (t->frames_before_disc_start)
        cyanrip_log(ctx, 0, "    Prepended:   %i frames of silence\n", t->frames_before_disc_start);
    cyanrip_log(ctx, 0,     "    Start LSN:   %i", t->start_lsn_sig);
    if (t->start_lsn != t->start_lsn_sig)
        cyanrip_log(ctx, 0, " (with offset: %i)\n", t->start_lsn);
    else
        cyanrip_log(ctx, 0, "\n");

    cyanrip_log(ctx, 0,     "    End LSN:     %i", t->end_lsn_sig);
    if (t->end_lsn != t->end_lsn_sig)
        cyanrip_log(ctx, 0, " (with offset: %i)\n", t->end_lsn);
    else
        cyanrip_log(ctx, 0, "\n");

    if (t->frames_after_disc_end)
        cyanrip_log(ctx, 0, "    Appended:    %i frames of silence\n", t->frames_after_disc_end);
}

void cyanrip_log_track_end(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char length[16];
    cyanrip_samples_to_duration(t->nb_samples, length);

    cyanrip_log(ctx, 0, "  Preemphasis:   ");
    if (!t->preemphasis) {
        cyanrip_log(ctx, 0, "none detected");

        if (ctx->settings.force_deemphasis)
            cyanrip_log(ctx, 0, " (deemphasis forced)\n");
        else
            cyanrip_log(ctx, 0, "\n");
    } else {
        if (t->preemphasis_in_subcode)
            cyanrip_log(ctx, 0, "present (subcode)");
        else
            cyanrip_log(ctx, 0, "present (TOC)");

        if (ctx->settings.deemphasis || ctx->settings.force_deemphasis)
            cyanrip_log(ctx, 0, " (deemphasis applied)\n");
        else
            cyanrip_log(ctx, 0, "\n");
    }

    cyanrip_log(ctx, 0, "\n  Properties:\n");

    if (t->track_is_data) {
        cyanrip_log(ctx, 0, "    Data bytes:  %i (%.2f Mib)\n",
                    t->frames*CDIO_CD_FRAMESIZE_RAW,
                    t->frames*CDIO_CD_FRAMESIZE_RAW / (1024.0 * 1024.0));
        cyanrip_log(ctx, 0, "    Frames:      %u\n", t->end_lsn_sig - t->start_lsn_sig + 1);
        print_offsets(ctx, t);
        cyanrip_log(ctx, 0, "\n");
        return;
    }

    cyanrip_log(ctx, 0, "    Duration:    %s\n", length);
    cyanrip_log(ctx, 0, "    Samples:     %u\n", t->nb_samples);
    cyanrip_log(ctx, 0, "    Frames:      %u\n", t->end_lsn_sig - t->start_lsn_sig + 1);

    print_offsets(ctx, t);

    int has_ar = t->ar_db_status == CYANRIP_ACCUDB_FOUND;

    if (t->computed_crcs) {
        cyanrip_log(ctx, 0, "\n  EAC CRC32:     %08X", t->eac_crc);
        if (t->total_repeats)
            cyanrip_log(ctx, 0, " (after %i rips)\n", t->total_repeats);
        else
            cyanrip_log(ctx, 0, "\n");
    }

    cyanrip_log(ctx, 0, "  Accurip:       %s",
                ctx->settings.disable_accurip ? "disabled" :
                has_ar ? "disc found in database" : "not found");
    if (has_ar)
        cyanrip_log(ctx, 0, " (max confidence: %i)\n", t->ar_db_max_confidence);
    else
        cyanrip_log(ctx, 0, "\n");

    if (t->computed_crcs) {
        int match_v1 = has_ar ? crip_find_ar(t, t->acurip_checksum_v1, 0) : 0;
        int match_v2 = has_ar ? crip_find_ar(t, t->acurip_checksum_v2, 0) : 0;

        cyanrip_log(ctx, 0, "    Accurip v1:  %08X", t->acurip_checksum_v1);
        if (has_ar && match_v1 > 0)
            cyanrip_log(ctx, 0, " (accurately ripped, confidence %i)\n", match_v1);
        else if (has_ar && (match_v2 < 1))
            cyanrip_log(ctx, 0, " (not found, either a new pressing, or bad rip)\n");
        else
            cyanrip_log(ctx, 0, "\n");

        cyanrip_log(ctx, 0, "    Accurip v2:  %08X", t->acurip_checksum_v2);
        if (has_ar && (match_v2 > 0))
            cyanrip_log(ctx, 0, " (accurately ripped, confidence %i)\n", match_v2);
        else if (has_ar && (match_v1 < 0))
            cyanrip_log(ctx, 0, " (not found, either a new pressing, or bad rip)\n");
        else
            cyanrip_log(ctx, 0, "\n");

        if (!has_ar || ((match_v1 < 0) && (match_v2 < 0))) {
            int match_450 = has_ar ? crip_find_ar(t, t->acurip_checksum_v1_450, 1) : 0;

            cyanrip_log(ctx, 0, "    Accurip 450: %08X", t->acurip_checksum_v1_450);
            if (has_ar && (match_450 > (3*(t->ar_db_max_confidence+1)/4)) && (t->acurip_checksum_v1_450 == 0x0)) {
                cyanrip_log(ctx, 0, " (match found, confidence %i, but a checksum of 0 is meaningless)\n",
                            match_450, t->ar_db_max_confidence);
            } else if (has_ar && (match_450 > (3*(t->ar_db_max_confidence+1)/4))) {
                cyanrip_log(ctx, 0, " (matches Accurip DB, confidence %i, track is partially accurately ripped)\n",
                            match_450, t->ar_db_max_confidence);
            } else if (has_ar) {
                cyanrip_log(ctx, 0, " (not found)\n");
            } else {
                cyanrip_log(ctx, 0, "\n");
            }
        }
    }

    cyanrip_log(ctx, 0, "\n  Metadata:\n", length);

    int max_key_len = 0;
    const AVDictionaryEntry *d = NULL;
    while ((d = av_dict_get(t->meta, "", d, AV_DICT_IGNORE_SUFFIX)))
        max_key_len = FFMAX(strlen(d->key), max_key_len);

    d = NULL;
    while ((d = av_dict_get(t->meta, "", d, AV_DICT_IGNORE_SUFFIX))) {
        int key_len = strlen(d->key);
        cyanrip_log(ctx, 0, "    %s: ", d->key);
        for (int i = 0; i < (max_key_len - key_len); i++)
            cyanrip_log(ctx, 0, " ");
        cyanrip_log(ctx, 0, "%s\n", d->value);
    }

    if (!ctx->settings.disable_coverart_embedding && (t->art.source_url || ctx->nb_cover_arts)) {
        const char *codec_name = NULL;
        CRIPArt *art = &t->art;
        if (!art->source_url) {
            int i;
            for (i = 0; i < ctx->nb_cover_arts; i++)
                if (!strcmp(dict_get(ctx->cover_arts[i].meta, "title"), "Front"))
                    break;
            art = &ctx->cover_arts[i == ctx->nb_cover_arts ? 0 : i];
        }

        if (art->pkt && art->params) {
            const AVCodecDescriptor *cd = avcodec_descriptor_get(art->params->codec_id);
            if (cd)
                codec_name = cd->long_name;
            else
                codec_name = avcodec_get_name(art->params->codec_id);
        }

        if (ctx->settings.print_info_only)
            cyanrip_log(ctx, 0, "\n  Embedded cover art:\n    %s: %s\n",
                        dict_get(art->meta, "title"), art->source_url);
        else
            cyanrip_log(ctx, 0, "\n  Embedded cover art:\n    %s: %ix%i %s\n",
                        dict_get(art->meta, "title"), art->params->width, art->params->height, codec_name);
    }

    cyanrip_log(ctx, 0, "\n  File(s):\n");
    for (int f = 0; f < ctx->settings.outputs_num; f++) {
        char *path = crip_get_path(ctx, CRIP_PATH_TRACK, 0,
                                   &crip_fmt_info[ctx->settings.outputs[f]],
                                   t);
        cyanrip_log(ctx, 0, "    %s\n", path);
        av_free(path);
    }

    cyanrip_log(ctx, 0, "\n");
}

void cyanrip_log_start_report(cyanrip_ctx *ctx)
{
    cyanrip_log(ctx, 0, "cyanrip %s (%s)\n", PROJECT_VERSION_STRING, vcstag);
    cyanrip_log(ctx, 0, "System device:  %s\n", ctx->settings.dev_path);
    if (ctx->drive->drive_model)
        cyanrip_log(ctx, 0, "Device model:   %s\n", ctx->drive->drive_model);
    cyanrip_log(ctx, 0, "Offset:         %c%i %s\n", ctx->settings.offset >= 0 ? '+' : '-', abs(ctx->settings.offset),
                abs(ctx->settings.offset) == 1 ? "sample" : "samples");
    cyanrip_log(ctx, 0, "%s%c%i %s\n",
                ctx->settings.over_under_read_frames < 0 ? "Underread:      " : "Overread:       ",
                ctx->settings.over_under_read_frames >= 0 ? '+' : '-',
                abs(ctx->settings.over_under_read_frames),
                abs(ctx->settings.over_under_read_frames) == 1 ? "frame" : "frames");
    cyanrip_log(ctx, 0, "%s%s\n",
                ctx->settings.over_under_read_frames < 0 ? "Underread mode: " : "Overread mode:  ",
                ctx->settings.overread_leadinout ? "read in lead-in/lead-out" : "fill with silence in lead-in/lead-out");
    if (ctx->settings.speed && (ctx->mcap & CDIO_DRIVE_CAP_MISC_SELECT_SPEED))
        cyanrip_log(ctx, 0, "Speed:          %ix\n", ctx->settings.speed);
    else
        cyanrip_log(ctx, 0, "Speed:          default (%s)\n",
                    (ctx->mcap & CDIO_DRIVE_CAP_MISC_SELECT_SPEED) ? "changeable" : "unchangeable");
    cyanrip_log(ctx, 0, "C2 errors:      %s by drive\n", (ctx->rcap & CDIO_DRIVE_CAP_READ_C2_ERRS) ?
                "supported" : "unsupported");
    if (ctx->settings.paranoia_level == crip_max_paranoia_level)
        cyanrip_log(ctx, 0, "Paranoia level: %s\n", "max");
    else if (ctx->settings.paranoia_level == 0)
        cyanrip_log(ctx, 0, "Paranoia level: %s\n", "none");
    else
        cyanrip_log(ctx, 0, "Paranoia level: %i\n", ctx->settings.paranoia_level);
    cyanrip_log(ctx, 0, "Frame retries:  %i\n", ctx->settings.max_retries);
    cyanrip_log(ctx, 0, "HDCD decoding:  %s\n", ctx->settings.decode_hdcd ? "enabled" : "disabled");

    cyanrip_log(ctx, 0, "Album Art:      %s", ctx->nb_cover_arts == 0 ? "none" : "");
    for (int i = 0; i < ctx->nb_cover_arts; i++) {
        const char *title = dict_get(ctx->cover_arts[i].meta, "title");
        const char *source = dict_get(ctx->cover_arts[i].meta, "source");
        cyanrip_log(ctx, 0, "%s%s%s%s%s", title,
                    source ? " (From: " : "",
                    source ? source : "",
                    source ? ")" : "",
                    i != (ctx->nb_cover_arts - 1) ? ", " : "");
    }
    cyanrip_log(ctx, 0, "\n");

    cyanrip_log(ctx, 0, "Outputs:        ");
    for (int i = 0; i < ctx->settings.outputs_num; i++)
        cyanrip_log(ctx, 0, "%s%s", cyanrip_fmt_desc(ctx->settings.outputs[i]), i != (ctx->settings.outputs_num - 1) ? ", " : "");
    cyanrip_log(ctx, 0, "\n");
    CLOG("Disc number:    %s\n", ctx->meta, "disc");
    CLOG("Total discs:    %s\n", ctx->meta, "totaldiscs");
    cyanrip_log(ctx, 0, "Disc tracks:    %i\n", ctx->nb_cd_tracks);
    cyanrip_log(ctx, 0, "Tracks to rip:  %s", (ctx->settings.rip_indices_count == -1) ? "all" : !ctx->settings.rip_indices_count ? "none" : "");
    if (ctx->settings.rip_indices_count != -1) {
        for (int i = 0; i < ctx->settings.rip_indices_count; i++)
            cyanrip_log(ctx, 0, "%i%s", ctx->settings.rip_indices[i], i != (ctx->settings.rip_indices_count - 1) ? ", " : "");
    }
    cyanrip_log(ctx, 0, "\n");

    char duration[16];
    cyanrip_frames_to_duration(ctx->duration_frames, duration);

    CLOG("DiscID:         %s\n", ctx->meta, "discid")
    CLOG("Release ID:     %s\n", ctx->meta, "release_id")
    CLOG("CDDB ID:        %s\n", ctx->meta, "cddb")
    CLOG("Disc MCN:       %s\n", ctx->meta, "disc_mcn")
    CLOG("Album:          %s\n", ctx->meta, "album")
    CLOG("Album artist:   %s\n", ctx->meta, "album_artist")

    cyanrip_log(ctx, 0, "AccurateRip:    %s\n", ctx->ar_db_status == CYANRIP_ACCUDB_ERROR ? "error" :
                                                ctx->ar_db_status == CYANRIP_ACCUDB_NOT_FOUND ? "not found" :
                                                ctx->ar_db_status == CYANRIP_ACCUDB_FOUND ? "found" :
                                                ctx->ar_db_status == CYANRIP_ACCUDB_MISMATCH ? "mismatch" :
                                                "disabled");

    cyanrip_log(ctx, 0, "Total time:     %s\n", duration);

    cyanrip_log(ctx, 0, "\n");
}

void cyanrip_log_finish_report(cyanrip_ctx *ctx)
{
    char t_s[64];
    time_t t_c = time(NULL);
    struct tm *t_l = localtime(&t_c);
    strftime(t_s, sizeof(t_s), "%Y-%m-%dT%H:%M:%S", t_l);

    if (ctx->ar_db_status == CYANRIP_ACCUDB_FOUND) {
        int accurip_verified = 0;
        int accurip_partial = 0;
        for (int i = 0; i < ctx->nb_tracks; i++) {
            cyanrip_track *t = &ctx->tracks[i];
            if (t->ar_db_status == CYANRIP_ACCUDB_FOUND) {
                if ((crip_find_ar(t, t->acurip_checksum_v1, 0) > 0) ||
                    (crip_find_ar(t, t->acurip_checksum_v2, 0) > 0))
                    accurip_verified++;
                else if (crip_find_ar(t, t->acurip_checksum_v1_450, 1) > (3*(t->ar_db_max_confidence+1)/4) &&
                         t->acurip_checksum_v1_450)
                    accurip_partial++;
            }
        }
        cyanrip_log(ctx, 0, "Tracks ripped accurately: %i/%i\n", accurip_verified, ctx->nb_tracks);
        if (accurip_partial)
            cyanrip_log(ctx, 0, "Tracks ripped partially accurately: %i/%i\n",
                        accurip_partial, ctx->nb_tracks - accurip_verified);
        cyanrip_log(ctx, 0, "\n");
    }

    int has_status = 0;
    cyanrip_log(ctx, 0, "Paranoia status counts:\n");

#define PCHECK(PROP)                                                           \
    if (paranoia_status[PARANOIA_CB_ ## PROP]) {                               \
        const char *pstr = "  " #PROP ": ";                                  \
        cyanrip_log(ctx, 0, "%s", pstr);                                       \
        int padding = strlen("  FIXUP_DROPPED: ") - strlen(pstr);            \
        for (int i = 0; i < padding; i++)                                      \
            cyanrip_log(ctx, 0, " ");                                          \
        cyanrip_log(ctx, 0, "%lu\n", paranoia_status[PARANOIA_CB_ ## PROP]);   \
        has_status |= !!paranoia_status[PARANOIA_CB_ ## PROP];                 \
    }

    PCHECK(READ)
    PCHECK(VERIFY)
    PCHECK(FIXUP_EDGE)
    PCHECK(FIXUP_ATOM)
    PCHECK(SCRATCH)
    PCHECK(REPAIR)
    PCHECK(SKIP)
    PCHECK(DRIFT)
    PCHECK(BACKOFF)
    PCHECK(OVERLAP)
    PCHECK(FIXUP_DROPPED)
    PCHECK(FIXUP_DUPED)
    PCHECK(READERR)
    PCHECK(CACHEERR)
    PCHECK(WROTE)
    PCHECK(FINISHED)
    cyanrip_log(ctx, 0, "%s\n", has_status ? "" : "  none\n");

#undef PCHECK

    cyanrip_log(ctx, 0, "Ripping errors: %i\n", ctx->total_error_count);
    cyanrip_log(ctx, 0, "Ripping finished at %s\n", t_s);
}

int cyanrip_log_init(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        char *logfile = crip_get_path(ctx, CRIP_PATH_LOG, 1,
                                      &crip_fmt_info[ctx->settings.outputs[i]],
                                      NULL);

        ctx->logfile[i] = av_fopen_utf8(logfile, "w+");

        if (!ctx->logfile[i]) {
            cyanrip_log(ctx, 0, "Couldn't open path \"%s\" for writing: %s!\n"
                        "Invalid folder name? Try -D <folder>.\n",
                        logfile, av_err2str(AVERROR(errno)));
            av_freep(&logfile);
            return 1;
        }

        av_freep(&logfile);
    }

    return 0;
}

void cyanrip_log_end(cyanrip_ctx *ctx)
{
    uint8_t digest[64];
    char digest_str[AV_BASE64_SIZE(64)];

    uint8_t *str_data = NULL;
    struct AVSHA512 *shactx = av_sha512_alloc();

    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        if (!ctx->logfile[i])
            continue;

        if (!shactx)
            goto fail;

        av_sha512_init(shactx, 512);

        long int pos = ftell(ctx->logfile[i]);
        uint8_t *str_data_new = av_realloc(str_data, pos);
        if (!str_data_new)
            goto fail;
        str_data = str_data_new;

        rewind(ctx->logfile[i]);
        long int read_bytes = fread(str_data, 1, pos, ctx->logfile[i]);
        fseek(ctx->logfile[i], 0, SEEK_END);

        av_sha512_update(shactx, str_data, read_bytes);
        av_sha512_final(shactx, digest);

        /* Proprietary top-secret FUN512 encrayptalignalaiton algorithm */
        for (int j = 0; j < 64; j++)         /* To wash a velociraptor... */
            digest[j] ^= 0x81 + i;           /* Stand behind it */
        for (int j = 0; j < 64; j++)         /* Proudly yell "I AM A TRAFFIC LIGHT SPECIALIST" */
            for (int k = 0; k < 64; k++)     /* A USB will descend, and quickly freeze the raptor */
                if (j != k)                  /* Carefully blast it with a jet engine to thaw it */
                    digest[j] ^= digest[k];  /* Enjoy your hot velociraptor meat by adding fresh miraculin */

        av_base64_encode(digest_str, AV_BASE64_SIZE(64), digest, 64);

        /* Pretend it's not base64 */
        for (int j = (AV_BASE64_SIZE(64) - 1); (digest_str[j] == '\0' || digest_str[j] == '='); j--)
            digest_str[j] = '\0';

        for (int j = 0; j < strlen(digest_str); j++) {
            if (digest_str[j] == '/') digest_str[j] = '_';
            if (digest_str[j] == '+') digest_str[j] = '.';
        }

        fprintf(ctx->logfile[i], "Log FUN512: %s\n", digest_str);
fail:
        fclose(ctx->logfile[i]);
        ctx->logfile[i] = NULL;
    }

    av_free(str_data);
    av_free(shactx);
}

static cyanrip_ctx *av_global_ctx = NULL;
static int av_max_log_level = AV_LOG_QUIET;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void av_log_capture(void *ptr, int lvl, const char *format,
                           va_list args)
{
    pthread_mutex_lock(&log_lock);

    if (lvl > av_max_log_level)
        goto end;

    if (av_global_ctx) {
        for (int i = 0; i < av_global_ctx->settings.outputs_num; i++) {
            if (!av_global_ctx->logfile[i])
                continue;

            va_list args2;
            va_copy(args2, args);
            vfprintf(av_global_ctx->logfile[i], format, args2);
            va_end(args2);
        }
    }

    vprintf(format, args);

end:
    pthread_mutex_unlock(&log_lock);
}

void cyanrip_set_av_log_capture(cyanrip_ctx *ctx, int enable,
                                int max_av_lvl)
{
    pthread_mutex_lock(&log_lock);

    if (enable) {
        av_global_ctx = ctx;
        av_max_log_level = max_av_lvl;
        av_log_set_callback(av_log_capture);
    } else {
        av_log_set_callback(av_log_default_callback);
        av_global_ctx = NULL;
        av_max_log_level = AV_LOG_QUIET;
    }

    pthread_mutex_unlock(&log_lock);
}

void cyanrip_log(cyanrip_ctx *ctx, int verbose, const char *format, ...)
{
    pthread_mutex_lock(&log_lock);

    va_list args;
    va_start(args, format);

    if (ctx) {
        for (int i = 0; i < ctx->settings.outputs_num; i++) {
            if (!ctx->logfile[i])
                continue;

            va_list args2;
            va_copy(args2, args);
            vfprintf(ctx->logfile[i], format, args2);
            va_end(args2);
        }
    }

    vprintf(format, args);

    va_end(args);

    pthread_mutex_unlock(&log_lock);
}
