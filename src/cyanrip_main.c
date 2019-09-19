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

#include <time.h>
#include <getopt.h>

#include "cyanrip_main.h"
#include "cyanrip_log.h"
#include "cyanrip_crc.h"
#include "cyanrip_encode.h"
#include "os_compat.h"

bool quit_now = 0;

void cyanrip_ctx_end(cyanrip_ctx **s)
{
    cyanrip_ctx *ctx;
    if (!s || !*s)
        return;
    ctx = *s;
    cyanrip_free_cover_image(ctx);
    for (int i = 0; i < ctx->drive->tracks; i++) {
        av_dict_free(&ctx->tracks[i].meta);
        av_freep(&ctx->tracks[i].base_data);
    }
    if (ctx->paranoia)
        cdio_paranoia_free(ctx->paranoia);
    if (ctx->drive)
        cdio_cddap_close_no_free_cdio(ctx->drive);
    if (ctx->cdio)
        cdio_destroy(ctx->cdio);
    av_dict_free(&ctx->meta);
    av_freep(&ctx->base_dst_folder);
    av_freep(&ctx->tracks);
    av_freep(&ctx);
    *s = NULL;
}

int cyanrip_ctx_init(cyanrip_ctx **s, cyanrip_settings *settings)
{
    int rval;
    char *error = NULL;

    cyanrip_ctx *ctx = av_mallocz(sizeof(cyanrip_ctx));

    ctx->settings = *settings;

    if (!ctx->settings.dev_path)
        ctx->settings.dev_path = cdio_get_default_device(NULL);

    if (!(ctx->cdio = cdio_open(ctx->settings.dev_path, DRIVER_UNKNOWN))) {
        cyanrip_log(ctx, 0, "Unable to init cdio context\n");
        return 1;
    }

    cdio_get_drive_cap(ctx->cdio, &ctx->rcap, &ctx->wcap, &ctx->mcap);

    if (!(ctx->drive = cdio_cddap_identify_cdio(ctx->cdio, 1, &error))) {
        cyanrip_log(ctx, 0, "Unable to init cddap context");
        if (error)
            cyanrip_log(ctx, 0, " - \"%s\"\n", error);
        else
            cyanrip_log(ctx, 0, "!\n");
        return 1;
    }

    cdio_cddap_verbose_set(ctx->drive, CDDA_MESSAGE_FORGETIT, CDDA_MESSAGE_FORGETIT);

    cyanrip_log(ctx, 0, "Opening drive...\n");
    rval = cdio_cddap_open(ctx->drive);
    if (rval < 0) {
        cyanrip_log(ctx, 0, "Unable to open device!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    if (ctx->mcap & CDIO_DRIVE_CAP_MISC_SELECT_SPEED)
        cdio_cddap_speed_set(ctx->drive, settings->speed);

    ctx->paranoia = cdio_paranoia_init(ctx->drive);
    if (!ctx->paranoia) {
        cyanrip_log(ctx, 0, "Unable to init paranoia!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    int mode = !ctx->settings.fast_mode ? PARANOIA_MODE_FULL : PARANOIA_MODE_DISABLE;
    cdio_paranoia_modeset(ctx->paranoia, mode);

    ctx->last_frame = cdio_get_track_lsn(ctx->cdio, CDIO_CDROM_LEADOUT_TRACK);
    if (ctx->drive->tracks)
        ctx->duration = cdio_get_track_last_lsn(ctx->cdio, ctx->drive->tracks) - cdio_get_track_last_lsn(ctx->cdio, 0);
    else
        ctx->duration = cdio_get_disc_last_lsn(ctx->cdio);

    ctx->tracks = av_calloc(cdda_tracks(ctx->drive) + 1, sizeof(cyanrip_track));

    for (int i = 0; i < ctx->drive->tracks; i++)
        ctx->tracks[i].number = i + 1;

    *s = ctx;
    return 0;
}

#define READ_MB(FUNC, MBCTX, DICT, KEY)                                        \
    do {                                                                       \
        int len = FUNC(MBCTX, NULL, 0) + 1;                                    \
        char *str = av_mallocz(4*len);                                         \
        FUNC(MBCTX, str, len);                                                 \
        av_dict_set(&DICT, KEY, str, AV_DICT_DONT_STRDUP_VAL | AV_DICT_APPEND);\
    } while (0)

void cyanrip_mb_credit(Mb5ArtistCredit credit, AVDictionary *dict, const char *key)
{
    Mb5NameCreditList namecredit_list = mb5_artistcredit_get_namecreditlist(credit);
    for (int i = 0; i < mb5_namecredit_list_size(namecredit_list); i++) {
        Mb5NameCredit namecredit = mb5_namecredit_list_item(namecredit_list, i);

        if (mb5_namecredit_get_name(namecredit, NULL, 0)) {
            READ_MB(mb5_namecredit_get_name, namecredit, dict, key);
        } else {
            Mb5Artist artist = mb5_namecredit_get_artist(namecredit);
            if (artist)
                READ_MB(mb5_artist_get_name, artist, dict, key);
        }

        READ_MB(mb5_namecredit_get_joinphrase, namecredit, dict, key);
    }
}

void cyanrip_mb_tracks(cyanrip_ctx *ctx, Mb5Release release, const char *discid)
{
    Mb5MediumList medium_list = mb5_release_media_matching_discid(release, discid);
    if (!medium_list) {
        cyanrip_log(ctx, 0, "No mediums matching DiscID.\n");
        return;
    }
    
    Mb5Medium medium = mb5_medium_list_item(medium_list, 0);
    if (!medium) {
        cyanrip_log(ctx, 0, "Got empty medium list.\n");
        goto end;
    }

    Mb5TrackList track_list = mb5_medium_get_tracklist(medium);
    if (!track_list) {
        cyanrip_log(ctx, 0, "Medium has no track list.\n");
        goto end;
    }

    for (int i = 0; i < mb5_track_list_size(track_list); i++) {
        if (i >= ctx->drive->tracks)
            break;
        Mb5Track track = mb5_track_list_item(track_list, i);
        Mb5Recording recording = mb5_track_get_recording(track);
        Mb5ArtistCredit credit;
        if (recording) {
            READ_MB(mb5_recording_get_title, recording, ctx->tracks[i].meta, "title");
            credit = mb5_recording_get_artistcredit(recording);
          } else {
            READ_MB(mb5_track_get_title, track, ctx->tracks[i].meta, "title");
            credit = mb5_track_get_artistcredit(track);
        }
        if (credit)
            cyanrip_mb_credit(credit, ctx->tracks[i].meta, "artist");
    }

end:
    mb5_medium_list_delete(medium_list);
}

int cyanrip_mb_metadata(cyanrip_ctx *ctx)
{
    int ret = 0;
    Mb5Query query = mb5_query_new("cyanrip", NULL, 0);
    if (!query) {
        cyanrip_log(ctx, 0, "Could not connect to MusicBrainz.\n");
        return 1;
    }

    char *names[] = { "inc" };
    char *values[] = { "recordings artist-credits" };
    const char *discid = dict_get(ctx->meta, "discid");
    Mb5Metadata metadata = mb5_query_query(query, "discid", discid, 0, 1, names, values);
    if (!metadata) {
        cyanrip_log(ctx, 0, "MusicBrainz lookup failed, either server was busy "
                            "or CD is missing from database, try again or disable with -n\n");
        ret = 1;
        goto end;
    }

    Mb5Disc disc = mb5_metadata_get_disc(metadata);
    if (!disc) {
        cyanrip_log(ctx, 0, "DiscID not found in MusicBrainz\n");
        goto end_meta;
    }

    Mb5ReleaseList release_list = mb5_disc_get_releaselist(disc);
    if (!release_list) {
        cyanrip_log(ctx, 0, "DiscID has no associated releases.\n");
        goto end_meta;
    }

    Mb5Release release = mb5_release_list_item(release_list, 0);
    if (!release) {
        cyanrip_log(ctx, 0, "No releases found for DiscID.\n");
        goto end_meta;
    }

    READ_MB(mb5_release_get_date, release, ctx->meta, "date");
    READ_MB(mb5_release_get_title, release, ctx->meta, "album");
    Mb5ArtistCredit artistcredit = mb5_release_get_artistcredit(release);
    if (artistcredit)
        cyanrip_mb_credit(artistcredit, ctx->meta, "album_artist");

    cyanrip_log(ctx, 0, "Found MusicBrainz release: %s - %s\n",
                dict_get(ctx->meta, "album"), dict_get(ctx->meta, "album_artist"));

    /* Read track metadata */
    cyanrip_mb_tracks(ctx, release, discid);

end_meta:
    mb5_metadata_delete(metadata);

end:
    mb5_query_delete(query);
    return ret;
}

int cyanrip_fill_metadata(cyanrip_ctx *ctx)
{
    int ret = 0;

    DiscId *discid = discid_new();
    if (!ctx->settings.fast_mode) {
        cyanrip_log(NULL, 0, "Reading full disc metadata (could take a while)...\n");
        if (!discid_read(discid, ctx->settings.dev_path)) {
            cyanrip_log(ctx, 0, "DiscID error: %s\n", discid_get_error_msg(discid));
        } else {
            av_dict_set(&ctx->meta, "discid", discid_get_id(discid), 0);
            av_dict_set(&ctx->meta, "disc_mcn", discid_get_mcn(discid), 0);

            /* MusicBrainz */
            if (!ctx->settings.disable_mb)
                ret |= cyanrip_mb_metadata(ctx);
        }
    } else {
        cyanrip_log(NULL, 0, "Extracting TOC...\n");
        if (!discid_read_sparse(discid, ctx->settings.dev_path, 0))
            cyanrip_log(ctx, 0, "DiscID error: %s\n", discid_get_error_msg(discid));
    }

    for (int i = 0; i < ctx->drive->tracks; i++) {
        if (discid && !(ctx->mcap & CDIO_DRIVE_CAP_MISC_FILE)) {
            const char *isrc = discid_get_track_isrc(discid, ctx->tracks[i].number);
            if (strlen(isrc))
                av_dict_set(&ctx->tracks[i].meta, "isrc", isrc, 0);
        }
    }

    if (discid)
        discid_free(discid);

    return ret;
}

static void cyanrip_copy_album_to_track_meta(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->drive->tracks; i++) {
        char t_s[64];
        time_t t_c = time(NULL);
        struct tm *t_l = localtime(&t_c);
        strftime(t_s, sizeof(t_s), "%Y-%m-%dT%H:%M:%S", t_l);

        av_dict_set(&ctx->tracks[i].meta, "creation_time", t_s, 0);
        av_dict_set(&ctx->tracks[i].meta, "comment", "cyanrip "CYANRIP_VERSION_STRING, 0);
        av_dict_set_int(&ctx->tracks[i].meta, "track", ctx->tracks[i].number, 0);
        av_dict_set_int(&ctx->tracks[i].meta, "tracktotal", ctx->drive->tracks, 0);
        av_dict_copy(&ctx->tracks[i].meta, ctx->meta, AV_DICT_DONT_OVERWRITE);
    }
}

bool cyanrip_read_frame(cyanrip_ctx *ctx, cyanrip_track *t)
{
    char *err = NULL;
    bool is_error = 0;
    int retries = ctx->settings.frame_max_retries;

    int16_t *samples = NULL;

    if (quit_now) {
        cyanrip_log(ctx, 0, "Quitting now!\n");
        cyanrip_ctx_end(&ctx);
        exit(1);
    }

    samples = cdio_paranoia_read_limited(ctx->paranoia, NULL, retries);

    if ((err = cdio_cddap_errors(ctx->drive))) {
        cyanrip_log(ctx, 0, "%s\n", err);
        free(err);
        err = NULL;
        is_error = 1;
    }

    if ((err = cdio_cddap_messages(ctx->drive))) {
        cyanrip_log(ctx, 0, "%s\n", err);
        free(err);
        err = NULL;
        is_error = 1;
    }

    if (!samples) {
        cyanrip_log(ctx, 0, "Frame read failed!\n");
        is_error = 1;
    } else {
        memcpy(t->base_data + t->nb_samples*2, samples, CDIO_CD_FRAMESIZE_RAW);
    }

    ctx->errors_count += is_error;
    t->nb_samples += CDIO_CD_FRAMESIZE_RAW >> 1;

    return is_error;
}

int cyanrip_rip_track(cyanrip_ctx *ctx, cyanrip_track *t)
{
    /* last frame obtained from cdio is inclusive */
    uint32_t last_frame = cdio_get_track_last_lsn(ctx->cdio, t->number);
    if (last_frame > ctx->last_frame) {
        cyanrip_log(ctx, 0, "Track last frame larger than last disc frame!\n");
        return 1;
    }
    uint32_t first_frame = cdio_get_track_lsn(ctx->cdio, t->number);
    uint32_t frames = last_frame - first_frame + 1;
    uint32_t samples = frames*(CDIO_CD_FRAMESIZE_RAW >> 1);

    t->start_sector = first_frame;
    t->end_sector = last_frame;

    frames += abs(ctx->settings.over_under_read_frames);

    int underread = ctx->settings.over_under_read_frames;
    underread = underread < 0 ? abs(underread) : 0;

    int overread = ctx->settings.over_under_read_frames;
    overread = overread > 0 ? overread : 0;

    lsn_t seek_dest = first_frame - underread;
    lsn_t prezero = seek_dest < 0 ? abs(seek_dest) : 0;
    seek_dest = seek_dest < 0 ? 0 : seek_dest;
    cdio_paranoia_seek(ctx->paranoia, seek_dest, SEEK_SET);

    t->preemphasis = cdio_get_track_preemphasis(ctx->cdio, t->number);

    t->base_data = av_mallocz(frames*CDIO_CD_FRAMESIZE_RAW);
    int offset = underread*CDIO_CD_FRAMESIZE_RAW + ctx->settings.offset*4;
    t->samples = (int16_t *)(t->base_data + offset);

    /* For underreading */
    t->nb_samples = (prezero*CDIO_CD_FRAMESIZE_RAW) >> 1;
    frames -= prezero;

    /* Don't overread into lead-out */
    if ((first_frame + frames) > ctx->last_frame)
        frames -= overread;

    int errors = 0;
    for (int i = 0; i < frames; i++) {
        errors += cyanrip_read_frame(ctx, t);
        cyanrip_log(NULL, 0, "\rRipping track %i, progress - %0.2f%%, errors - %i", t->number, ((double)i/frames)*100.0f, errors);
    }
    cyanrip_log(NULL, 0, "\r\nTrack %i ripped!\n", t->number);

    t->nb_samples = samples;
    cyanrip_crc_track(ctx, t);

    int enc_errs = ctx->errors_count;

    for (int i = 0; i < ctx->settings.outputs_num; i++)
        ctx->errors_count += cyanrip_encode_track(ctx, t, ctx->settings.outputs[i]);

    enc_errs = ctx->errors_count - enc_errs;
    if (enc_errs) {
        cyanrip_log(ctx, 0, "Failed to encode track %i!\n", t->number);
        return 1;
    }

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

int main(int argc, char **argv)
{
    cyanrip_ctx *ctx = NULL;
    cyanrip_settings settings;

    if (signal(SIGINT, on_quit_signal) == SIG_ERR)
        cyanrip_log(ctx, 0, "Can't init signal handler!\n");

    /* Default settings */
    settings.dev_path = NULL;
    settings.base_dst_folder = NULL;
    settings.cover_image_path = NULL;
    settings.verbose = 1;
    settings.speed = 0;
    settings.fast_mode = 0;
    settings.frame_max_retries = 5;
    settings.over_under_read_frames = 0;
    settings.offset = 0;
    settings.disable_mb = 0;
    settings.bitrate = 128.0f;
    settings.rip_indices_count = -1;
    settings.outputs[0] = CYANRIP_FORMAT_FLAC;
    settings.outputs_num = 1;

    int c;
    char *p;
    char *album_metadata_ptr = NULL;
    char *track_metadata_ptr[99] = { NULL };
    int track_metadata_ptr_cnt = 0;

    while ((c = getopt(argc, argv, "hnfVl:a:t:b:c:r:d:o:s:S:D:")) != -1) {
        switch (c) {
        case 'h':
            cyanrip_log(ctx, 0, "cyanrip %s help:\n", CYANRIP_VERSION_STRING);
            cyanrip_log(ctx, 0, "    -d <path>            Set device path\n");
            cyanrip_log(ctx, 0, "    -D <path>            Folder to rip disc to\n");
            cyanrip_log(ctx, 0, "    -c <path>            Set cover image path\n");
            cyanrip_log(ctx, 0, "    -s <int>             CD Drive offset in samples\n");
            cyanrip_log(ctx, 0, "    -S <int>             Drive speed\n");
            cyanrip_log(ctx, 0, "    -o <string>          Comma separated list of outputs\n");
            cyanrip_log(ctx, 0, "    -b <kbps>            Bitrate of lossy files in kbps\n");
            cyanrip_log(ctx, 0, "    -l <list>            Select which tracks to rip\n");
            cyanrip_log(ctx, 0, "    -r <int>             Maximum number of retries to read a frame\n");
            cyanrip_log(ctx, 0, "    -a <string>          Album metadata, key=value:key=value\n");
            cyanrip_log(ctx, 0, "    -t <number>=<string> Track metadata, can be specified multiple times\n");
            cyanrip_log(ctx, 0, "    -f                   Disable all error checking\n");
            cyanrip_log(ctx, 0, "    -V                   Print program version\n");
            cyanrip_log(ctx, 0, "    -h                   Print options help\n");
            cyanrip_log(ctx, 0, "    -n                   Disable musicbrainz lookup\n");
            return 0;
            break;
        case 'S':
            settings.speed = abs((int)strtol(optarg, NULL, 10));
            break;
        case 'r':
            settings.frame_max_retries = strtol(optarg, NULL, 10);
            break;
        case 's':
            settings.offset = strtol(optarg, NULL, 10);
            int sign = settings.offset < 0 ? -1 : +1;
            int frames = ceilf(abs(settings.offset)/(float)(CDIO_CD_FRAMESIZE_RAW >> 2));
            settings.over_under_read_frames = sign*frames;
            break;
        case 'n':
            settings.disable_mb = 1;
            break;
        case 'b':
            settings.bitrate = strtof(optarg, NULL);
            break;
        case 'l':
            settings.rip_indices_count = 0;
            p = strtok(optarg, ",");
            while(p != NULL) {
                settings.rip_indices[settings.rip_indices_count++] = strtol(p, NULL, 10);
                if (!settings.rip_indices[settings.rip_indices_count - 1]) {
                    settings.rip_indices_count = 0;
                    break;
                }
                p = strtok(NULL, ",");
            }
            break;
        case 'o':
            settings.outputs_num = 0;
            if (!strncmp("help", optarg, strlen("help"))) {
                cyanrip_log(ctx, 0, "Supported output codecs:\n");
                cyanrip_print_codecs();
                return 0;
            }
            p = strtok(optarg, ",");
            while (p != NULL) {
                int res = cyanrip_validate_fmt(p);
                if (res != -1) {
                    settings.outputs[settings.outputs_num++] = res;
                } else {
                    cyanrip_log(ctx, 0, "Invalid format \"%s\"\n", p);
                    return 1;
                }
                p = strtok(NULL, ",");
            }
            break;
        case 'c':
            settings.cover_image_path = optarg;
            break;
        case 'D':
            settings.base_dst_folder = optarg;
            break;
        case 'V':
            cyanrip_log(ctx, 0, "cyanrip %s\n", CYANRIP_VERSION_STRING);
            return 0;
        case 'd':
            settings.dev_path = optarg;
            break;
        case 'f':
            settings.fast_mode = 1;
            break;
        case '?':
            return 1;
            break;
        case 'a':
            album_metadata_ptr = optarg;
            break;
        case 't':
            track_metadata_ptr[track_metadata_ptr_cnt++] = optarg;
            break;
        default:
            abort();
            break;
        }
    }

    if (cyanrip_ctx_init(&ctx, &settings))
        return 1;

    if (cyanrip_fill_metadata(ctx))
        return 1;

    /* Read user album metadata */
    if (album_metadata_ptr) {
        int err = av_dict_parse_string(&ctx->meta, album_metadata_ptr,
                                       "=", ":", 0);
        if (err) {
            cyanrip_log(ctx, 0, "Error reading album tags: %s\n",
                        av_err2str(err));
            return 1;
        }
    }

    cyanrip_copy_album_to_track_meta(ctx);

    /* Read user track metadata */
    for (int i = 0; i < track_metadata_ptr_cnt; i++) {
        if (!track_metadata_ptr[i])
            continue;

        char *end = NULL;
        int track_num = strtol(track_metadata_ptr[i], &end, 10) - 1;
        if (track_num < 0 || track_num >= ctx->drive->tracks) {
            cyanrip_log(ctx, 0, "Invalid track number %i\n", track_num + 1);
            return 1;
        }

        int err = av_dict_parse_string(&ctx->tracks[track_num].meta,
                                       end + 1, "=", ":", 0);
        if (err) {
            cyanrip_log(ctx, 0, "Error reading track tags: %s\n",
                        av_err2str(err));
            return 1;
        }
    }

    if (ctx->settings.base_dst_folder)
        ctx->base_dst_folder = av_strdup(ctx->settings.base_dst_folder);
    else if (dict_get(ctx->meta, "album"))
        ctx->base_dst_folder = cyanrip_sanitize_fn(dict_get(ctx->meta, "album"));
    else if (dict_get(ctx->meta, "discid"))
        ctx->base_dst_folder = cyanrip_sanitize_fn(dict_get(ctx->meta, "discid"));
    else
        ctx->base_dst_folder = av_strdup("Untitled CD");

    cyanrip_read_cover_image(ctx);

    cyanrip_log_init(ctx);
    cyanrip_log_start_report(ctx);

    if (ctx->settings.rip_indices_count == -1) {
        for (int i = 0; i < ctx->drive->tracks; i++)
            ctx->success |= cyanrip_rip_track(ctx, &ctx->tracks[i]);
    } else {
        for (int i = 0; i < ctx->settings.rip_indices_count; i++) {
            int index = ctx->settings.rip_indices[i] - 1;
            if (index < 0 || index >= ctx->drive->tracks)
                continue;
            ctx->success |= cyanrip_rip_track(ctx, &ctx->tracks[index]);
        }
    }

    int ret = ctx->success;

    cyanrip_log_finish_report(ctx);
    cyanrip_log_end(ctx);

    cyanrip_ctx_end(&ctx);

    return ret;
}

#ifdef HAVE_WMAIN
int wmain(int argc, wchar_t *argv[])
{
    char *argstr_flat, **win32_argv_utf8 = NULL;
    int i, ret, buffsize = 0, offset = 0;

    /* determine the UTF-8 buffer size (including NULL-termination symbols) */
    for (i = 0; i < argc; i++)
        buffsize += WideCharToMultiByte(CP_UTF8, 0, argv[i], -1,
                                        NULL, 0, NULL, NULL);

    win32_argv_utf8 = av_mallocz(sizeof(char *) * (argc + 1) + buffsize);
    argstr_flat     = (char *)win32_argv_utf8 + sizeof(char *) * (argc + 1);

    for (i = 0; i < argc; i++) {
        win32_argv_utf8[i] = &argstr_flat[offset];
        offset += WideCharToMultiByte(CP_UTF8, 0, argv[i], -1,
                                      &argstr_flat[offset],
                                      buffsize - offset, NULL, NULL);
    }
    win32_argv_utf8[i] = NULL;

    ret = main(argc, win32_argv_utf8);

    av_free(win32_argv_utf8);
    return ret;
}
#endif
