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
#include "cyanrip_log.h"

bool quit_now = 0;

void cyanrip_ctx_end(cyanrip_ctx **s)
{
    cyanrip_ctx *ctx;
    if (!s || !*s)
        return;
    ctx = *s;
    for (int i = 0; i < ctx->drive->tracks; i++)
        free(ctx->tracks[i].samples);
    cyanrip_end_encoding(ctx);
    if (ctx->paranoia)
        cdio_paranoia_free(ctx->paranoia);
    if (ctx->drive)
        cdio_cddap_close_no_free_cdio(ctx->drive);
    if (ctx->disc_mcn)
        free(ctx->disc_mcn);
    if (ctx->cdio)
        cdio_destroy(ctx->cdio);
    free(ctx->tracks);
    free(ctx);
    *s = NULL;
}

int cyanrip_ctx_init(cyanrip_ctx **s, cyanrip_settings *settings)
{
    int rval;
    char *error = NULL;

    cyanrip_ctx *ctx = calloc(1, sizeof(cyanrip_ctx));

    if (!(ctx->cdio = cdio_open(ctx->settings.dev_path, DRIVER_UNKNOWN))) {
        cyanrip_log(ctx, 0, "Unable to init cdio context");
        return 1;
    }

    ctx->drive = cdio_cddap_identify_cdio(ctx->cdio, 1, &error);
    if (!ctx->drive) {
        cyanrip_log(ctx, 0, "Unable to init cddap context");
        if (error)
            cyanrip_log(ctx, 0, " - \"%s\"\n", error);
        else
            cyanrip_log(ctx, 0, "!\n");
        cyanrip_ctx_end(&ctx);
        return 1;
    }

    cdio_cddap_verbose_set(ctx->drive, CDDA_MESSAGE_FORGETIT, CDDA_MESSAGE_FORGETIT);

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

    if (ctx->drive->tracks) {
        ctx->duration = cdio_get_track_last_lsn(ctx->cdio, ctx->drive->tracks) - cdio_get_track_last_lsn(ctx->cdio, 0);
        ctx->last_frame = cdio_get_track_last_lsn(ctx->cdio, ctx->drive->tracks);
    } else {
        ctx->duration = cdio_get_disc_last_lsn(ctx->cdio);
        ctx->last_frame = ctx->duration;
    }

    cyanrip_init_encoding(ctx);

    ctx->tracks = calloc(cdda_tracks(ctx->drive) + 1, sizeof(cyanrip_track));

    ctx->last_frame = cdio_cddap_disc_lastsector(ctx->drive);

    ctx->settings = *settings;

    *s = ctx;
    return 0;
}

void cyanrip_mb_credit(Mb5ArtistCredit credit, char *s, int len) {
    Mb5NameCreditList namecredit_list = mb5_artistcredit_get_namecreditlist(credit);
    int c = 0;
    for (int i = 0; i < mb5_namecredit_list_size(namecredit_list); i++) {
        Mb5NameCredit namecredit = mb5_namecredit_list_item(namecredit_list, i);
        if (mb5_namecredit_get_name(namecredit, &s[c], len - c)) {
            c += mb5_namecredit_get_name(namecredit, &s[c], len - c);
        } else {
            Mb5Artist artist = mb5_namecredit_get_artist(namecredit);
            if (artist)
                c += mb5_artist_get_name(artist, &s[c], len - c);
        }
        c += mb5_namecredit_get_joinphrase(namecredit, &s[c], len - c);
    }
}

void cyanrip_mb_tracks(cyanrip_ctx *ctx, Mb5Release release)
{
    Mb5MediumList medium_list = mb5_release_media_matching_discid(release, ctx->discid);
    if (medium_list) {
        Mb5Medium medium = mb5_medium_list_item(medium_list, 0);
        if (medium) {
            Mb5TrackList track_list = mb5_medium_get_tracklist(medium);
            if (track_list) {
                for (int i = 0; i < mb5_track_list_size(track_list); i++) {
                    if (i >= ctx->drive->tracks)
                        continue;
                    Mb5Track track = mb5_track_list_item(track_list, i);
                    Mb5Recording recording = mb5_track_get_recording(track);
                    Mb5ArtistCredit credit;
                    if (recording) {
                        mb5_recording_get_title(recording, ctx->tracks[i].name, 255);
                        credit = mb5_recording_get_artistcredit(recording);
                      } else {
                        mb5_track_get_title(track, ctx->tracks[i].name, 255);
                        credit = mb5_track_get_artistcredit(track);
                    }
                    if (credit)
                        cyanrip_mb_credit(credit, ctx->tracks[i].artist, 255);
                }
            } else {
                cyanrip_log(ctx, 0, "Medium has no track list.\n");
            }
        } else {
            cyanrip_log(ctx, 0, "Got empty medium list.\n");
        }
        mb5_medium_list_delete(medium_list);
    } else {
        cyanrip_log(ctx, 0, "No mediums matching DiscID.\n");
    }
}

int cyanrip_mb_metadata(cyanrip_ctx *ctx)
{
    int ret = 0;
    Mb5Query query = mb5_query_new("cyanrip", NULL, 0);
    if (query) {
        char* names[] = { "inc" };
        char* values[] = { "recordings artist-credits" };
        Mb5Metadata metadata = mb5_query_query(query, "discid", ctx->discid, 0, 1, names, values);
        if (metadata) {
            Mb5Disc disc = mb5_metadata_get_disc(metadata);
            if (disc) {
                Mb5ReleaseList release_list = mb5_disc_get_releaselist(disc);
                if (release_list) {
                    Mb5Release release = mb5_release_list_item(release_list, 0);
                    if (release) {
                        mb5_release_get_title(release, ctx->disc_name, 255);
                        Mb5ArtistCredit artistcredit = mb5_release_get_artistcredit(release);
                        if (artistcredit)
                            cyanrip_mb_credit(artistcredit, ctx->album_artist, 255);
                        cyanrip_log(ctx, 0, "Found MusicBrainz release: %s - %s\n",
                                    ctx->disc_name, ctx->album_artist);
                        cyanrip_mb_tracks(ctx, release);
                    } else {
                        cyanrip_log(ctx, 0, "No releases found for DiscID.\n");
                    }
                } else {
                    cyanrip_log(ctx, 0, "DiscID has no associated releases.\n");
                }
             } else {
                cyanrip_log(ctx, 0, "DiscID not found in MusicBrainz\n");
            }
            mb5_metadata_delete(metadata);
        } else {
            cyanrip_log(ctx, 0, "MusicBrainz lookup failed, try again later.\n");
            ret = 1;
        }
        mb5_query_delete(query);
    } else {
        cyanrip_log(ctx, 0, "Could not connect to MusicBrainz.\n");
        ret = 1;
    }

    return ret;
}

int cyanrip_fill_metadata(cyanrip_ctx *ctx)
{
    int ret = 0;

    ctx->disc_mcn = cdio_get_mcn(ctx->cdio);

    /* Album time */
    time_t t_c = time(NULL);
    ctx->disc_date = localtime(&t_c);

    /* DiscID */
    DiscId *disc = discid_new();
    if (discid_read_sparse(disc, ctx->settings.dev_path, 0) == 0)
        cyanrip_log(ctx, 0, "DiscID error: %s\n", discid_get_error_msg(disc));
    else
        strcpy(ctx->discid, discid_get_id(disc));
    discid_free(disc);

    /* MusicBrainz */
    if (!ctx->settings.disable_mb)
        ret |= cyanrip_mb_metadata(ctx);

    return ret;
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

int cyanrip_read_track(cyanrip_ctx *ctx, cyanrip_track *t, int index)
{
    uint32_t frames = 0, first_frame = 0;

    t->index = index;

    if (index < ctx->drive->tracks) {
        frames += cdio_get_track_last_lsn(ctx->cdio, t->index + 1);
        if (frames > ctx->last_frame) {
            cyanrip_log(ctx, 0, "Track last frame larger than last disc frame!\n");
            return 1;
        }
        first_frame = cdio_get_track_lsn(ctx->cdio, t->index + 1);
        frames -= first_frame;
    } else {
        cyanrip_log(ctx, 0, "Invalid track index = %i\n", index);
        return 1;
    }

    frames += 2*OVER_UNDER_READ_FRAMES;

#ifdef HAVE_CDIO_PARANOIA_PARANOIA_H
    strcpy(t->isrc, mmc_get_track_isrc(ctx->cdio, t->index + 1));
#else
    mmc_isrc_track_read_subchannel(ctx->cdio, t->index + 1, t->isrc);
#endif

    cdio_paranoia_seek(ctx->paranoia, first_frame - OVER_UNDER_READ_FRAMES, SEEK_SET);

    t->preemphasis = cdio_get_track_preemphasis(ctx->cdio, t->index + 1);

    t->samples = calloc(frames*CDIO_CD_FRAMESIZE_RAW, 1);
    t->nb_samples = 0;

    for (int i = 0; i < frames; i++) {
        cyanrip_read_frame(ctx, t);
        if (!(i % ctx->settings.report_rate))
            cyanrip_log(NULL, 0, "\rTrack %i progress - %0.2f%%", t->index + 1, ((double)i/frames)*100.0f);
    }
    cyanrip_log(NULL, 0, "\r\nTrack %i ripped!\n", t->index + 1);

    t->nb_samples -= OVER_UNDER_READ_FRAMES*CDIO_CD_FRAMESIZE_RAW;

    cyanrip_crc_track(ctx, t);

    for (int i = 0; i < ctx->settings.outputs_num; i++)
        cyanrip_encode_track(ctx, t, ctx->settings.outputs[i]);

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
    int ret;

    cyanrip_ctx *ctx;
    cyanrip_settings settings;

    if (signal(SIGINT, on_quit_signal) == SIG_ERR)
        cyanrip_log(NULL, 0, "Can't init signal handler!\n");

    settings.dev_path = "/dev/sr0";
    settings.cover_image_path = "";
    settings.verbose = 1;
    settings.speed = 0;
    settings.frame_max_retries = 5;
    settings.paranoia_mode = PARANOIA_MODE_FULL;
    settings.report_rate = 20;
    settings.offset = 0;
    settings.disable_mb = 0;
    settings.bitrate = 128.0f;
    settings.rip_indices_count = -1;

    /* Debug */
    settings.outputs[0] = CYANRIP_FORMAT_FLAC;
    settings.outputs_num = 1;

    int c;
    char *p;
    while((c = getopt (argc, argv, "hnft:b:c:r:d:o:s:")) != -1) {
        switch (c) {
            case 'h':
                cyanrip_log(NULL, 0, "cyanrip help:\n");
                cyanrip_log(NULL, 0, "    -d <path>    Set device path\n");
                cyanrip_log(NULL, 0, "    -c <path>    Set cover image path\n");
                cyanrip_log(NULL, 0, "    -s <int>     CD Drive offset\n");
                cyanrip_log(NULL, 0, "    -o <string>  Comma separated list of outputs\n");
                cyanrip_log(NULL, 0, "    -b <kbps>    Bitrate of lossy files in kbps\n");
                cyanrip_log(NULL, 0, "    -t <list>    Select which tracks to rip\n");
                cyanrip_log(NULL, 0, "    -r <int>     Maximum number of retries to read a frame\n");
                cyanrip_log(NULL, 0, "    -f           Disable all error checking\n");
                cyanrip_log(NULL, 0, "    -h           Print options help\n");
                cyanrip_log(NULL, 0, "    -n           Disable musicbrainz lookup\n");
                return 0;
                break;
            case 'r':
                settings.frame_max_retries = strtol(optarg, NULL, 10);
                break;
            case 's':
                settings.offset = strtol(optarg, NULL, 10);
                if (abs(settings.offset)*4 > OVER_UNDER_READ_FRAMES*CDIO_CD_FRAMESIZE_RAW) {
                    cyanrip_log(NULL, 0, "Drive offset too large!\n");
                    abort();
                }
                break;
            case 'n':
                settings.disable_mb = 1;
                break;
            case 'b':
                settings.bitrate = strtof(optarg, NULL);
                break;
            case 't':
                settings.rip_indices_count = 0;
                p = strtok(optarg, ",");
                while(p != NULL) {
                    settings.rip_indices[settings.rip_indices_count++] = strtol(p, NULL, 10);
                    p = strtok(NULL, ",");
                }
                break;
            case 'o':
                settings.outputs_num = 0;
                if (!strncmp("help", optarg, strlen("help"))) {
                    cyanrip_log(NULL, 0, "Supported codecs:\n");
                    cyanrip_print_codecs();
                    return 0;
                }
                p = strtok(optarg, ",");
                while(p != NULL) {
                    int res = cyanrip_validate_fmt(p);
                    if (res != -1) {
                        settings.outputs[settings.outputs_num++] = res;
                    } else {
                        cyanrip_log(NULL, 0, "Invalid format \"%s\"\n", p);
                        return 1;
                    }
                    p = strtok(NULL, ",");
                }
                break;
            case 'c':
                settings.cover_image_path = optarg;
                break;
            case 'd':
                settings.dev_path = optarg;
                break;
            case 'f':
                settings.paranoia_mode = PARANOIA_MODE_DISABLE;
                break;
            case '?':
                return 1;
                break;
            default:
                abort();
                break;
        }
    }

    if ((ret = cyanrip_ctx_init(&ctx, &settings)))
        return ret;

    if (cyanrip_fill_metadata(ctx))
        return 1;

    cyanrip_setup_cover_image(ctx);

    cyanrip_log_init(ctx);
    cyanrip_log_start_report(ctx);

    ret = 0;

    if (ctx->settings.rip_indices_count == -1) {
        for (int i = 0; i < ctx->drive->tracks; i++)
            ret |= cyanrip_read_track(ctx, &ctx->tracks[i], i);
    } else {
        for (int i = 0; i < ctx->settings.rip_indices_count; i++) {
            int index = ctx->settings.rip_indices[i] - 1;
            if (index < 0 || index >= ctx->drive->tracks)
                continue;
            ret |= cyanrip_read_track(ctx, &ctx->tracks[index], index);
        }
    }

    cyanrip_log_finish_report(ctx);
    cyanrip_log_end(ctx);

    cyanrip_ctx_end(&ctx);

    return ret;
}
