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
#include <sys/stat.h>

#include <libavutil/avutil.h>
#include "cyanrip_encode.h"
#include "cyanrip_log.h"
#include "os_compat.h"

#define CLOG(FORMAT, DICT, TAG)                                                \
    if (dict_get(DICT, TAG))                                                   \
        cyanrip_log(ctx, 0, FORMAT, dict_get(DICT, TAG));                      \

void cyanrip_log_track_end(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char length[16];
    cyanrip_samples_to_duration(t->nb_samples, length);

    if (t->track_is_data) {
        cyanrip_log(ctx, 0, "    Data bytes:       %i\n\n", t->frames*CDIO_CD_FRAMESIZE_RAW);
        return;
    }

    cyanrip_log(ctx, 0, "    Metadata:\n", length);

    const AVDictionaryEntry *d = NULL;
    while ((d = av_dict_get(t->meta, "", d, AV_DICT_IGNORE_SUFFIX)))
        cyanrip_log(ctx, 0, "        %s: %s\n", d->key, d->value);

    if (t->preemphasis)
        cyanrip_log(ctx, 0, "    Preemphasis:      present, deemphasis required\n");
    cyanrip_log(ctx, 0, "    Duration:         %s\n", length);
    cyanrip_log(ctx, 0, "    Samples:          %u\n", t->nb_samples);

    if (t->pregap_lsn != CDIO_INVALID_LSN)
        cyanrip_log(ctx, 0, "    Pregap LSN:       %i\n", t->pregap_lsn);

    if (t->frames_before_disc_start)
        cyanrip_log(ctx, 0, "    Silent frames:    %i prepended\n", t->frames_before_disc_start);
    cyanrip_log(ctx, 0, "    Start LSN:        %i\n", t->start_lsn_sig);
    if (t->start_lsn != t->start_lsn_sig)
        cyanrip_log(ctx, 0, "    Offset start:     %i\n", t->start_lsn);

    cyanrip_log(ctx, 0, "    End LSN:          %i\n", t->end_lsn_sig);
    if (t->end_lsn != t->end_lsn_sig)
        cyanrip_log(ctx, 0, "    Offset end:       %i\n", t->end_lsn);
    if (t->frames_after_disc_end)
        cyanrip_log(ctx, 0, "    Silent frames:    %i appended\n", t->frames_after_disc_end);

    int has_ar = t->ar_db_status == CYANRIP_ACCUDB_FOUND;

    if (!ctx->settings.disable_accurip) {
        cyanrip_log(ctx, 0, "    Accurip:          %s", has_ar ? "found" : "not found");
        if (has_ar)
            cyanrip_log(ctx, 0, " (confidence: %i)\n", t->ar_db_confidence);
        else
            cyanrip_log(ctx, 0, "\n");
    }

    if (t->computed_crcs) {
        cyanrip_log(ctx, 0, "    EAC CRC32:        0x%08x\n", t->eac_crc);

        int match_v1 = has_ar ? t->ar_db_checksum == t->acurip_checksum_v1 : 0;
        int match_v2 = has_ar ? t->ar_db_checksum == t->acurip_checksum_v2 : 0;

        cyanrip_log(ctx, 0, "    Accurip v1:       0x%08x", t->acurip_checksum_v1);
        if (has_ar && match_v1)
            cyanrip_log(ctx, 0, " (accurately ripped)\n");
        else if (has_ar && !match_v2)
            cyanrip_log(ctx, 0, " (doesn't match checksum in Accurip DB of 0x%x)\n", t->ar_db_checksum);
        else
            cyanrip_log(ctx, 0, "\n");

        cyanrip_log(ctx, 0, "    Accurip v2:       0x%08x", t->acurip_checksum_v2);
        if (has_ar && match_v2)
            cyanrip_log(ctx, 0, " (accurately ripped)\n");
        else if (has_ar && !match_v1)
            cyanrip_log(ctx, 0, " (doesn't match checksum in Accurip DB of 0x%x)\n", t->ar_db_checksum);
        else
            cyanrip_log(ctx, 0, "\n");

        if (!has_ar || (!match_v1 && !match_v2)) {
            cyanrip_log(ctx, 0, "    Accurip v1 450:   0x%08x", t->ar_db_checksum_450);
            if (has_ar && t->acurip_checksum_v1_450 == t->ar_db_checksum_450)
                cyanrip_log(ctx, 0, " (matches Accurip DB, track is partially accurately ripped)\n");
            else if (has_ar)
                cyanrip_log(ctx, 0, " (doesn't match checksum in Accurip DB of 0x%x)\n", t->acurip_checksum_v1_450);
            else
                cyanrip_log(ctx, 0, "\n");
        }
    } else if (has_ar) {
        cyanrip_log(ctx, 0, "    Accurip checksum: 0x%08x (in database)\n", t->ar_db_checksum);
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
    cyanrip_log(ctx, 0, "Frame retries:  %i\n", ctx->settings.frame_max_retries);
    cyanrip_log(ctx, 0, "HDCD decoding:  %s\n", ctx->settings.decode_hdcd ? "enabled" : "disabled");
    CLOG("Album Art:      %s\n", ctx->meta, "cover_art")
    cyanrip_log(ctx, 0, "Base folder:    %s\n", ctx->base_dst_folder);
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
                if (t->ar_db_checksum == t->acurip_checksum_v1 ||
                    t->ar_db_checksum == t->acurip_checksum_v2)
                    accurip_verified++;
                else if (t->ar_db_checksum_450 == t->acurip_checksum_v1_450)
                    accurip_partial++;
            }
        }
        cyanrip_log(ctx, 0, "Tracks ripped accurately: %i/%i\n", accurip_verified, ctx->nb_tracks);
        if (accurip_partial)
            cyanrip_log(ctx, 0, "Tracks ripped partially accurately: %i/%i\n",
                        accurip_partial, ctx->nb_tracks - accurip_verified);
        cyanrip_log(ctx, 0, "\n");
    }

    cyanrip_log(ctx, 0, "Paranoia status counts:\n");

#define PCHECK(PROP)                                                           \
    if (paranoia_status[PARANOIA_CB_ ## PROP]) {                               \
        const char *pstr = "    " #PROP ": ";                                  \
        cyanrip_log(ctx, 0, "%s", pstr);                                       \
        int padding = strlen("    FIXUP_DROPPED: ") - strlen(pstr);            \
        for (int i = 0; i < padding; i++)                                      \
            cyanrip_log(ctx, 0, " ");                                          \
        cyanrip_log(ctx, 0, "%lu\n", paranoia_status[PARANOIA_CB_ ## PROP]);   \
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
    cyanrip_log(ctx, 0, "\n");

#undef PCHECK

    cyanrip_log(ctx, 0, "Ripping errors: %i\n", ctx->total_error_count);
    cyanrip_log(ctx, 0, "Ripping finished at %s\n", t_s);
}

int cyanrip_log_init(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        /* Directory name */
        char *dirname = av_asprintf("%s [%s]", ctx->base_dst_folder,
                                    cyanrip_fmt_folder(ctx->settings.outputs[i]));

        /* Create if it doesn't exist */
        struct stat st_req = { 0 };
        if (stat(dirname, &st_req) == -1)
            mkdir(dirname, 0700);

        /* Log file name */
        char *logfile;
        const char *discnumber = dict_get(ctx->meta, "disc");
        if (discnumber)
            logfile = av_asprintf("%s/%s CD%s.log", dirname, ctx->base_dst_folder, discnumber);
        else
            logfile = av_asprintf("%s/%s.log", dirname, ctx->base_dst_folder);

        av_freep(&dirname);

        ctx->logfile[i] = av_fopen_utf8(logfile, "w");

        if (!ctx->logfile) {
            cyanrip_log(ctx, 0, "Error opening log file \"%s\" to write to!\n", logfile);
            av_freep(&logfile);
            return 1;
        }

        av_freep(&logfile);
    }

    return 0;
}

void cyanrip_log_end(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        if (!ctx->logfile[i])
            continue;

        fclose(ctx->logfile[i]);
        ctx->logfile[i] = NULL;
    }
}

static cyanrip_ctx *av_global_ctx = NULL;
static int av_log_indent = 0;
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

            for (int j = 0; j < av_log_indent; j++)
                fprintf(av_global_ctx->logfile[i], "    ");

            va_list args2;
            va_copy(args2, args);
            vfprintf(av_global_ctx->logfile[i], format, args2);
            va_end(args2);
        }
    }

    for (int i = 0; i < av_log_indent; i++)
        printf("    ");

    vprintf(format, args);

end:
    pthread_mutex_unlock(&log_lock);
}

void cyanrip_set_av_log_capture(cyanrip_ctx *ctx, int enable,
                                int indent, int max_av_lvl)
{
    pthread_mutex_lock(&log_lock);

    if (enable) {
        av_global_ctx = ctx;
        av_max_log_level = max_av_lvl;
        av_log_indent = indent;
        av_log_set_callback(av_log_capture);
    } else {
        av_log_set_callback(av_log_default_callback);
        av_global_ctx = NULL;
        av_log_indent = 0;
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
