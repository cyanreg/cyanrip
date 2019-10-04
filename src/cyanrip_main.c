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

int quit_now = 0;

static void cyanrip_ctx_end(cyanrip_ctx **s)
{
    cyanrip_ctx *ctx;
    if (!s || !*s)
        return;
    ctx = *s;

    for (int i = 0; i < ctx->nb_tracks; i++)
        av_dict_free(&ctx->tracks[i].meta);

    if (ctx->paranoia)
        cdio_paranoia_free(ctx->paranoia);
    if (ctx->drive)
        cdio_cddap_close_no_free_cdio(ctx->drive);
    if (ctx->settings.eject_on_success_rip && !ctx->total_error_count &&
        (ctx->mcap & CDIO_DRIVE_CAP_MISC_EJECT) && ctx->cdio && !quit_now)
        cdio_eject_media(&ctx->cdio);
    else if (ctx->cdio)
        cdio_destroy(ctx->cdio);

    av_dict_free(&ctx->meta);
    av_freep(&ctx->base_dst_folder);
    av_freep(&ctx);

    *s = NULL;
}

static int cyanrip_ctx_init(cyanrip_ctx **s, cyanrip_settings *settings)
{
    cyanrip_ctx *ctx = av_mallocz(sizeof(cyanrip_ctx));

    memcpy(&ctx->settings, settings, sizeof(cyanrip_settings));

    if (ctx->settings.print_info_only)
        ctx->settings.eject_on_success_rip = 0;

    if (!ctx->settings.dev_path)
        ctx->settings.dev_path = cdio_get_default_device(NULL);

    if (!(ctx->cdio = cdio_open(ctx->settings.dev_path, DRIVER_UNKNOWN))) {
        cyanrip_log(ctx, 0, "Unable to init cdio context\n");
        return AVERROR(EINVAL);
    }

    cdio_get_drive_cap(ctx->cdio, &ctx->rcap, &ctx->wcap, &ctx->mcap);

    char *msg = NULL;
    if (!(ctx->drive = cdio_cddap_identify_cdio(ctx->cdio, CDDA_MESSAGE_LOGIT, &msg))) {
        cyanrip_log(ctx, 0, "Unable to init cddap context!\n");
        if (msg) {
            cyanrip_log(ctx, 0, "cdio: \"%s\"\n", msg);
            cdio_cddap_free_messages(msg);
        }
        return AVERROR(EINVAL);
    }

    if (msg) {
        cyanrip_log(ctx, 0, "%s\n", msg);
        cdio_cddap_free_messages(msg);
    }

    cyanrip_log(ctx, 0, "Opening drive...\n");
    int ret = cdio_cddap_open(ctx->drive);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to open device!\n");
        cyanrip_ctx_end(&ctx);
        return AVERROR(EINVAL);
    }

    cdio_cddap_verbose_set(ctx->drive, CDDA_MESSAGE_LOGIT, CDDA_MESSAGE_FORGETIT);

    if (settings->speed) {
        if (!(ctx->mcap & CDIO_DRIVE_CAP_MISC_SELECT_SPEED)) {
            cyanrip_log(ctx, 0, "Device does not support changing speeds!\n");
            cyanrip_ctx_end(&ctx);
            return AVERROR(EINVAL);
        }

        ret = cdio_cddap_speed_set(ctx->drive, settings->speed);
        msg = cdio_cddap_errors(ctx->drive);
        if (msg) {
            cyanrip_log(ctx, 0, "cdio error: %s\n", msg);
            cdio_cddap_free_messages(msg);
        }
        if (ret)
            return AVERROR(EINVAL);
    }

    /* Drives are very slow and burst-y so don't block by default */
    if (!ctx->settings.enc_fifo_size && (ctx->mcap & CDIO_DRIVE_CAP_MISC_FILE))
        ctx->settings.enc_fifo_size = 16;

    ctx->paranoia = cdio_paranoia_init(ctx->drive);
    if (!ctx->paranoia) {
        cyanrip_log(ctx, 0, "Unable to init paranoia!\n");
        cyanrip_ctx_end(&ctx);
        return AVERROR(EINVAL);
    }

    if (ctx->mcap & CDIO_DRIVE_CAP_MISC_FILE)
        cdio_paranoia_modeset(ctx->paranoia, PARANOIA_MODE_DISABLE);
    else
        cdio_paranoia_modeset(ctx->paranoia, PARANOIA_MODE_FULL);

    ctx->start_lsn = 0;

    ctx->end_lsn = cdio_get_track_lsn(ctx->cdio, CDIO_CDROM_LEADOUT_TRACK) - 1;
    ctx->duration_frames = ctx->end_lsn - ctx->start_lsn + 1;

    ctx->nb_tracks = cdio_cddap_tracks(ctx->drive);
    if ((ctx->nb_tracks < 1) || (ctx->nb_tracks > CDIO_CD_MAX_TRACKS)) {
        cyanrip_log(ctx, 0, "Invalid number of tracks: %i!\n", ctx->nb_tracks);
        ctx->nb_tracks = 0;
        cyanrip_ctx_end(&ctx);
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < ctx->nb_tracks; i++)
        ctx->tracks[i].number = i + 1;

    /* For hot removal detection - init this so we can detect changes */
    cdio_get_media_changed(ctx->cdio);

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

static void mb_credit(Mb5ArtistCredit credit, AVDictionary *dict, const char *key)
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

static void mb_tracks(cyanrip_ctx *ctx, Mb5Release release, const char *discid)
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
        if (i >= ctx->nb_tracks)
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
            mb_credit(credit, ctx->tracks[i].meta, "artist");
    }

end:
    mb5_medium_list_delete(medium_list);
}

static int mb_metadata(cyanrip_ctx *ctx, int manual_metadata_specified)
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
        tQueryResult res = mb5_query_get_lastresult(query);
        if (res != eQuery_ResourceNotFound) {
            int chars = mb5_query_get_lasterrormessage(query, NULL, 0) + 1;
            char *msg = av_mallocz(chars*sizeof(*msg));
            mb5_query_get_lasterrormessage(query, msg, chars);
            cyanrip_log(ctx, 0, "MusicBrainz lookup failed: %s\n", msg);
            av_freep(&msg);
        }

        switch(res) {
        case eQuery_Timeout:
        case eQuery_ConnectionError:
            cyanrip_log(ctx, 0, "Connection failed, try again? Or disable via -n\n");
            break;
        case eQuery_AuthenticationError:
        case eQuery_FetchError:
        case eQuery_RequestError:
            cyanrip_log(ctx, 0, "Error fetching/requesting/auth, this shouldn't happen.\n");
            break;
        case eQuery_ResourceNotFound:
            if (manual_metadata_specified) {
                cyanrip_log(ctx, 0, "Unable to find metadata for this CD, but "
                            "metadata has been manually specified, continuing.\n");
                goto end;
            } else {
                cyanrip_log(ctx, 0, "Unable to find release info for this CD, "
                            "and metadata hasn't been manually added!\n");
                cyanrip_log(ctx, 0, "To continue add metadata via -a or -t, or ignore via -n!\n");
            }
            break;
        default:
            break;
        }

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
        mb_credit(artistcredit, ctx->meta, "album_artist");

    cyanrip_log(ctx, 0, "Found MusicBrainz release: %s - %s\n",
                dict_get(ctx->meta, "album"), dict_get(ctx->meta, "album_artist"));

    /* Read track metadata */
    mb_tracks(ctx, release, discid);

end_meta:
    mb5_metadata_delete(metadata);

end:
    mb5_query_delete(query);
    return ret;
}

static int fill_metadata(cyanrip_ctx *ctx, int manual_metadata_specified)
{
    /* Get disc MCN */
    if (ctx->rcap & CDIO_DRIVE_CAP_READ_MCN) {
        const char *mcn = cdio_get_mcn(ctx->cdio);
        if (mcn) {
            if (strlen(mcn))
                av_dict_set(&ctx->meta, "disc_mcn", mcn, 0);
            cdio_free((void *)mcn);
        }
    }

    if (ctx->mcap & CDIO_DRIVE_CAP_MISC_FILE)
        return 0;

    /* Get discid */
    DiscId *discid = discid_new();
    if (!discid_read_sparse(discid, ctx->settings.dev_path, 0)) {
        cyanrip_log(ctx, 0, "Error reading discid: %s!\n",
                    discid_get_error_msg(discid));
        return 1;
    }

    const char *disc_id_str = discid_get_id(discid);
    av_dict_set(&ctx->meta, "discid", disc_id_str, 0);
    discid_free(discid);

    /* Get musicbrainz tags */
    if (!ctx->settings.disable_mb)
        return mb_metadata(ctx, manual_metadata_specified);

    return 0;
}

static void copy_album_to_track_meta(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->nb_tracks; i++) {
        char t_s[64];
        time_t t_c = time(NULL);
        struct tm *t_l = localtime(&t_c);
        strftime(t_s, sizeof(t_s), "%Y-%m-%dT%H:%M:%S", t_l);

        av_dict_set(&ctx->tracks[i].meta, "creation_time", t_s, 0);
        av_dict_set(&ctx->tracks[i].meta, "comment", "cyanrip "CYANRIP_VERSION_STRING, 0);
        av_dict_set_int(&ctx->tracks[i].meta, "track", ctx->tracks[i].number, 0);
        av_dict_set_int(&ctx->tracks[i].meta, "tracktotal", ctx->nb_tracks, 0);
        av_dict_copy(&ctx->tracks[i].meta, ctx->meta, AV_DICT_DONT_OVERWRITE);
    }
}

static const uint8_t silent_frame[CDIO_CD_FRAMESIZE_RAW] = { 0 };

uint64_t paranoia_status[PARANOIA_CB_FINISHED + 1] = { 0 };

static void status_cb(long int n, paranoia_cb_mode_t status)
{
    if (status >= PARANOIA_CB_READ && status <= PARANOIA_CB_FINISHED)
        paranoia_status[status]++;
}

static const uint8_t *cyanrip_read_frame(cyanrip_ctx *ctx, cyanrip_track *t)
{
    int err = 0;
    char *msg = NULL;

    const uint8_t *data;
    data = (void *)cdio_paranoia_read_limited(ctx->paranoia, &status_cb,
                                              ctx->settings.frame_max_retries);

    msg = cdio_cddap_errors(ctx->drive);
    if (msg) {
        cyanrip_log(ctx, 0, "\ncdio error: %s\n", msg);
        cdio_cddap_free_messages(msg);
        err = 1;
    }

    if (!data) {
        if (!msg) {
            cyanrip_log(ctx, 0, "\nFrame read failed!\n");
            err = 1;
        }
        data = silent_frame;
    }

    ctx->total_error_count += err;

    return data;
}

static void track_read_extra(cyanrip_ctx *ctx, cyanrip_track *t)
{
    if (!t->track_is_data) {
        t->preemphasis = cdio_cddap_track_preemp(ctx->drive, t->number);
        if (t->preemphasis)
            av_dict_set(&t->meta, "deemphasis", "required", 0);

        if (ctx->rcap & CDIO_DRIVE_CAP_READ_ISRC) {
            const char *isrc_str = cdio_get_track_isrc(ctx->cdio, t->number);
            if (isrc_str) {
                if (strlen(isrc_str))
                    av_dict_set(&t->meta, "isrc", isrc_str, 0);
                cdio_free((void *)isrc_str);
            }
        }
    }
}

static int cyanrip_rip_track(cyanrip_ctx *ctx, cyanrip_track *t)
{
    int ret = 0;

    const int frames_before_disc_start = t->frames_before_disc_start;
    const int frames = t->frames;
    const int frames_after_disc_end = t->frames_after_disc_end;
    const ptrdiff_t offs = t->partial_frame_byte_offs;

    if (t->track_is_data) {
        cyanrip_log(ctx, 0, "Track %i is data, skipping:\n", t->number);
        cyanrip_log_track_end(ctx, t);
        return 0;
    }

    /* Hopefully reduce seeking by reading this here */
    track_read_extra(ctx, t);

    cdio_paranoia_seek(ctx->paranoia, t->start_lsn, SEEK_SET);

    cyanrip_dec_ctx *dec_ctx = { NULL };
    cyanrip_enc_ctx *enc_ctx[CYANRIP_FORMATS_NB] = { NULL };
    ret = cyanrip_create_dec_ctx(ctx, &dec_ctx, t);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error initializing decoder!\n");
        goto fail;
    }
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        ret = cyanrip_init_track_encoding(ctx, &enc_ctx[i], dec_ctx, t,
                                          ctx->settings.outputs[i]);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error initializing encoder!\n");
            goto fail;
        }
    }

    int start_err = ctx->total_error_count;

    /* Checksum */
    cyanrip_crc_ctx crc_ctx;
    init_crc_ctx(ctx, &crc_ctx, t);

    /* Fill with silence to maintain track length */
    for (int i = 0; i < frames_before_disc_start; i++) {
        int bytes = CDIO_CD_FRAMESIZE_RAW;
        const uint8_t *data = silent_frame;

        if (!i && offs) {
            data += CDIO_CD_FRAMESIZE_RAW + offs;
            bytes = -offs;
        }

        process_crc(&crc_ctx, data, bytes);
        ret = cyanrip_send_pcm_to_encoders(ctx, enc_ctx, ctx->settings.outputs_num,
                                           dec_ctx, data, bytes);
        if (ret) {
            cyanrip_log(ctx, 0, "Error in decoding/sending frame!\n");
            goto fail;
        }
    }

    /* Read the actual CD data */
    for (int i = 0; i < frames; i++) {
        /* Detect disc removals */
        if (cdio_get_media_changed(ctx->cdio)) {
            cyanrip_log(ctx, 0, "\nDrive media changed, stopping!\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }

        int bytes = CDIO_CD_FRAMESIZE_RAW;
        const uint8_t *data = cyanrip_read_frame(ctx, t);

        /* Account for partial frames caused by the offset */
        if (offs > 0) {
            if (!i) {
                data  += offs;
                bytes -= offs;
            } else if ((i == (frames - 1)) && !frames_after_disc_end) {
                bytes = offs;
            }
        } else if (offs < 0) {
            if (!i && !frames_before_disc_start) {
                data += CDIO_CD_FRAMESIZE_RAW + offs;
                bytes = -offs;
            } else if (i == (frames - 1)) {
                bytes += offs;
            }
        }

        /* Stop now if requested */
        if (quit_now) {
            cyanrip_log(ctx, 0, "\nStopping, ripping incomplete!\n");
            break;
        }

        /* Update CRCs */
        process_crc(&crc_ctx, data, bytes);

        /* Decode and encode */
        ret = cyanrip_send_pcm_to_encoders(ctx, enc_ctx, ctx->settings.outputs_num,
                                           dec_ctx, data, bytes);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "\nError in decoding/sending frame!\n");
            goto fail;
        }

        /* Report progress */
        cyanrip_log(NULL, 0, "\rRipping and encoding track %i, progress - %0.2f%%",
                    t->number, ((double)(i + 1)/frames)*100.0f);
        if (ctx->total_error_count - start_err)
            cyanrip_log(NULL, 0, ", errors - %i", ctx->total_error_count - start_err);
    }

    /* Fill with silence to maintain track length */
    for (int i = 0; i < frames_after_disc_end; i++) {
        int bytes = CDIO_CD_FRAMESIZE_RAW;
        const uint8_t *data = silent_frame;

        if ((i == (frames_after_disc_end - 1)) && offs)
            bytes = offs;

        process_crc(&crc_ctx, data, bytes);
        ret = cyanrip_send_pcm_to_encoders(ctx, enc_ctx, ctx->settings.outputs_num,
                                           dec_ctx, data, bytes);
        if (ret) {
            cyanrip_log(ctx, 0, "Error in decoding/sending frame!\n");
            goto fail;
        }
    }

    finalize_crc(&crc_ctx, t);

    cyanrip_log(NULL, 0, "\nFlushing encoders...\n");

    /* Flush encoders */
    ret = cyanrip_send_pcm_to_encoders(ctx, enc_ctx, ctx->settings.outputs_num,
                                       dec_ctx, NULL, 0);
    if (ret)
        cyanrip_log(ctx, 0, "Error sending flush signal to encoders!\n");

fail:
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        int err = cyanrip_end_track_encoding(&enc_ctx[i]);
        if (err) {
            cyanrip_log(ctx, 0, "Error in encoding!\n");
            ret = err;
            break;
        }
    }

    if (!ret && !quit_now)
        cyanrip_log(ctx, 0, "Track %i ripped and encoded successfully!\n", t->number);

    cyanrip_free_dec_ctx(ctx, &dec_ctx);

    if (!ret)
        cyanrip_log_track_end(ctx, t);
    else
        ctx->total_error_count++;

    return ret;
}

static void on_quit_signal(int signo)
{
    if (quit_now) {
        cyanrip_log(NULL, 0, "Force quitting\n");
        exit(1);
    }
    cyanrip_log(NULL, 0, "\r\nTrying to quit\n");
    quit_now = 1;
}

static void setup_track_lsn(cyanrip_ctx *ctx, cyanrip_track *t)
{
    lsn_t first_frame = t->start_lsn;
    lsn_t last_frame  = t->end_lsn;

    /* Duration doesn't depend on adjustments we make to frames */
    int frames = last_frame - first_frame + 1;

    t->nb_samples = frames*(CDIO_CD_FRAMESIZE_RAW >> 2);

    /* Move the seek position coarsely */
    const int extra_frames = ctx->settings.over_under_read_frames;
    int sign = (extra_frames < 0) ? -1 : ((extra_frames > 0) ? +1 : 0);
    first_frame += sign*FFMAX(FFABS(extra_frames) - 1, 0);
    last_frame += sign*FFMAX(FFABS(extra_frames) - 1, 0);

    /* Bump the lower/higher frame in the offset direction */
    first_frame -= sign < 0;
    last_frame  += sign > 0;

    /* Don't read into the lead in/out */
    if (!ctx->settings.overread_leadinout) {
        t->frames_before_disc_start = FFMAX(ctx->start_lsn - first_frame, 0);
        t->frames_after_disc_end = FFMAX(last_frame - ctx->end_lsn, 0);

        first_frame += t->frames_before_disc_start;
        last_frame  -= t->frames_after_disc_end;
    } else {
        t->frames_before_disc_start = 0;
        t->frames_after_disc_end = 0;
    }

    /* Offset accounted start/end sectors */
    t->start_lsn = first_frame;
    t->end_lsn = last_frame;
    t->frames = last_frame - first_frame + 1;

    /* Last/first frame partial offset */
    ptrdiff_t offs = ctx->settings.offset*4;
    offs -= sign*FFMAX(FFABS(extra_frames) - 1, 0)*CDIO_CD_FRAMESIZE_RAW;

    t->partial_frame_byte_offs = offs;
}

static void setup_track_offsets_and_report(cyanrip_ctx *ctx)
{
    int gaps = 0;

    for (int i = 0; i < ctx->nb_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];

        t->number = i + 1;
        t->track_is_data = !cdio_cddap_track_audiop(ctx->drive, t->number);
        if (t->track_is_data) {
            ctx->settings.pregap_action[t->number - 1] = CYANRIP_PREGAP_MERGE;
            ctx->settings.pregap_action[t->number - 0] = CYANRIP_PREGAP_DROP;
        }
        t->pregap_lsn = cdio_get_track_pregap_lsn(ctx->cdio, t->number);
        t->start_lsn = cdio_get_track_lsn(ctx->cdio, t->number);
        t->end_lsn = cdio_get_track_last_lsn(ctx->cdio, t->number);

        t->start_lsn_sig = t->start_lsn;
        t->end_lsn_sig = t->end_lsn;
    }

    cyanrip_log(ctx, 0, "Gaps:\n");

    /* Before pregap */
    if (ctx->tracks[0].pregap_lsn != CDIO_INVALID_LSN &&
        ctx->tracks[0].pregap_lsn > ctx->start_lsn) {
        cyanrip_log(ctx, 0, "    %i frame gap between lead-in and track 1 pregap, merging into pregap\n",
                    ctx->tracks[0].pregap_lsn - ctx->start_lsn);
        gaps++;

        ctx->tracks[0].pregap_lsn = ctx->start_lsn;
    } else if (ctx->tracks[0].pregap_lsn == CDIO_INVALID_LSN &&
               ctx->tracks[0].start_lsn > ctx->start_lsn) {
        cyanrip_log(ctx, 0, "    %i frame unmarked gap between lead-in and track 1, marking as a pregap\n",
                    ctx->tracks[0].start_lsn - ctx->start_lsn);
        gaps++;

        ctx->tracks[0].pregap_lsn = ctx->start_lsn;
    }

    /* Pregaps */
    for (int i = 0; i < ctx->nb_tracks; i++) {
        cyanrip_track *ct = &ctx->tracks[i - 0];
        cyanrip_track *lt = ct->number > 1 ? &ctx->tracks[i - 1] : NULL;

        if (ct->pregap_lsn == CDIO_INVALID_LSN)
            continue;

        cyanrip_log(ctx, 0, "    %i frame pregap in track %i, ",
                    ct->start_lsn - ct->pregap_lsn, ct->number);
        gaps++;

        switch (ctx->settings.pregap_action[ct->number - 1]) {
        case CYANRIP_PREGAP_DEFAULT:
            if (!lt)
                cyanrip_log(ctx, 0, "unmerged\n");
            else
                cyanrip_log(ctx, 0, "merging into track %i\n", lt->number);
            break;
        case CYANRIP_PREGAP_DROP:
            cyanrip_log(ctx, 0, "dropping\n");
            if (lt)
                lt->end_lsn = ct->pregap_lsn - 1;
            break;
        case CYANRIP_PREGAP_MERGE:
            cyanrip_log(ctx, 0, "merging\n");
            ct->start_lsn = ct->pregap_lsn;
            if (lt)
                lt->end_lsn = ct->pregap_lsn - 1;
            break;
        case CYANRIP_PREGAP_TRACK:
            cyanrip_log(ctx, 0, "splitting off into a new track, number %i\n", ct->number);

            if (lt)
                lt->end_lsn = ct->pregap_lsn - 1;

            for (int j = i; j < ctx->nb_tracks; j++)
                ctx->tracks[j].number++;

            memmove(&ctx->tracks[i + 1], &ctx->tracks[i],
                    sizeof(cyanrip_track)*(ctx->nb_tracks - i));

            ct = &ctx->tracks[i + 1];

            ctx->nb_tracks++;
            cyanrip_track *nt = &ctx->tracks[i];

            memset(nt, 0, sizeof(*nt));

            nt->number = ct->number - 1;
            nt->pregap_lsn = CDIO_INVALID_LSN;
            nt->start_lsn = ct->pregap_lsn;
            nt->end_lsn = ct->start_lsn - 1;

            ct->pregap_lsn = CDIO_INVALID_LSN;
        }
    }

    /* Between tracks */
    for (int i = 1; i < ctx->nb_tracks; i++) {
        cyanrip_track *ct = &ctx->tracks[i - 0];
        cyanrip_track *lt = &ctx->tracks[i - 1];

        if (ct->start_lsn == (lt->end_lsn + 1))
            continue;

        int discont_frames = ct->start_lsn - lt->end_lsn;

        cyanrip_log(ctx, 0, "    %i frame discontinuity between tracks %i and %i, ",
                    discont_frames, ct->number, lt->number);
        gaps++;

        if (ctx->settings.pregap_action[ct->number - 1] != CYANRIP_PREGAP_DROP) {
            cyanrip_log(ctx, 0, "padding track %i\n", lt->number);
            lt->end_lsn = ct->start_lsn - 1;
        } else {
            cyanrip_log(ctx, 0, "ignoring\n");
        }
    }

    /* After last track */
    cyanrip_track *lt = &ctx->tracks[ctx->nb_tracks - 1];
    if (ctx->end_lsn > lt->end_lsn) {
        int discont_frames = ctx->end_lsn - lt->end_lsn;
        cyanrip_log(ctx, 0, "    %i frame gap between last track and lead-out, padding track\n",
                    discont_frames);
        gaps++;

        lt->end_lsn = ctx->end_lsn;
    }

    /* Finally set up the internals with the set start_lsn/end_lsn */
    for (int i = 0; i < ctx->nb_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];

        if (t->track_is_data)
            t->frames = t->end_lsn - t->start_lsn + 1;
        else
            setup_track_lsn(ctx, t);
    }

    cyanrip_log(ctx, 0, "%s\n", gaps ? "" : "    None signalled\n");
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
    settings.speed = 0;
    settings.frame_max_retries = 25;
    settings.over_under_read_frames = 0;
    settings.offset = 0;
    settings.print_info_only = 0;
    settings.disable_mb = 0;
    settings.decode_hdcd = 0;
    settings.bitrate = 128.0f;
    settings.overread_leadinout = 0;
    settings.rip_indices_count = -1;
    settings.enc_fifo_size = 0;
    settings.eject_on_success_rip = 0;
    settings.outputs[0] = CYANRIP_FORMAT_FLAC;
    settings.outputs_num = 1;

    memset(settings.pregap_action, 0, sizeof(settings.pregap_action));

    int c;
    char *p;
    char *cover_image_path = NULL;
    char *album_metadata_ptr = NULL;
    char *track_metadata_ptr[99] = { NULL };
    int track_metadata_ptr_cnt = 0;

    while ((c = getopt(argc, argv, "hnHIVEOl:a:t:b:c:r:d:o:s:S:D:p:")) != -1) {
        switch (c) {
        case 'h':
            cyanrip_log(ctx, 0, "cyanrip %s help:\n", CYANRIP_VERSION_STRING);
            cyanrip_log(ctx, 0, "\n  Ripping options:\n");
            cyanrip_log(ctx, 0, "    -d <path>             Set device path\n");
            cyanrip_log(ctx, 0, "    -s <int>              CD Drive offset in samples (default: 0)\n");
            cyanrip_log(ctx, 0, "    -r <int>              Maximum number of retries to read a frame (default: 25)\n");
            cyanrip_log(ctx, 0, "    -S <int>              Set drive speed (default: unset)\n");
            cyanrip_log(ctx, 0, "    -p <number>=<string>  Track pregap handling (default: default)\n");
            cyanrip_log(ctx, 0, "    -O                    Enable overreading into lead-in and lead-out\n");
            cyanrip_log(ctx, 0, "    -H                    Enable HDCD decoding. Do this if you're sure disc is HDCD\n");
            cyanrip_log(ctx, 0, "\n  Metadata options:\n");
            cyanrip_log(ctx, 0, "    -I                    Only print CD and track info\n");
            cyanrip_log(ctx, 0, "    -a <string>           Album metadata, key=value:key=value\n");
            cyanrip_log(ctx, 0, "    -t <number>=<string>  Track metadata, can be specified multiple times\n");
            cyanrip_log(ctx, 0, "    -c <path>             Set cover image path\n");
            cyanrip_log(ctx, 0, "    -n                    Disables MusicBrainz lookup and ignores lack of manual metadata\n");
            cyanrip_log(ctx, 0, "\n  Output options:\n");
            cyanrip_log(ctx, 0, "    -l <list>             Select which tracks to rip (default: all)\n");
            cyanrip_log(ctx, 0, "    -D <path>             Base folder name to rip disc to\n");
            cyanrip_log(ctx, 0, "    -o <string>           Comma separated list of outputs\n");
            cyanrip_log(ctx, 0, "    -b <kbps>             Bitrate of lossy files in kbps\n");
            cyanrip_log(ctx, 0, "\n  Misc. options:\n");
            cyanrip_log(ctx, 0, "    -E                    Eject tray once successfully done\n");
            cyanrip_log(ctx, 0, "    -V                    Print program version\n");
            cyanrip_log(ctx, 0, "    -h                    Print options help\n");
            return 0;
            break;
        case 'S':
            settings.speed = abs((int)strtol(optarg, NULL, 10));
            if (settings.speed < 0) {
                cyanrip_log(ctx, 0, "Invalid drive speed!\n");
                return 1;
            }
            break;
        case 'r':
            settings.frame_max_retries = strtol(optarg, NULL, 10);
            if (settings.frame_max_retries < 0) {
                cyanrip_log(ctx, 0, "Invalid retries amount!\n");
                return 1;
            }
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
                int idx = strtol(p, NULL, 10);
                for (int i = 0; i < settings.rip_indices_count; i++) {
                    if (settings.rip_indices[i] == idx) {
                        cyanrip_log(ctx, 0, "Duplicated rip idx %i\n", idx);
                        return 1;
                    }
                }
                settings.rip_indices[settings.rip_indices_count++] = idx;
                p = strtok(NULL, ",");
            }
            qsort(settings.rip_indices, settings.rip_indices_count,
                  sizeof(int), cmp_numbers);
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
                for (int i = 0; i < settings.outputs_num; i++) {
                    if (settings.outputs[i] == res) {
                        cyanrip_log(ctx, 0, "Duplicated format \"%s\"\n", p);
                        return 1;
                    }
                }
                if (res != -1) {
                    settings.outputs[settings.outputs_num++] = res;
                } else {
                    cyanrip_log(ctx, 0, "Invalid format \"%s\"\n", p);
                    return 1;
                }
                p = strtok(NULL, ",");
            }
            break;
        case 'I':
            settings.print_info_only = 1;
            break;
        case 'H':
            settings.decode_hdcd = 1;
            break;
        case 'O':
            settings.overread_leadinout = 1;
            break;
        case 'p':
            p = strtok(optarg, "=");
            int idx = strtol(p, NULL, 10);
            if (idx < 1 || idx > 99) {
                cyanrip_log(ctx, 0, "Invalid track idx %i\n", idx);
                return 1;
            }
            enum cyanrip_pregap_action act = CYANRIP_PREGAP_DEFAULT;
            p = strtok(NULL, "=");
            if (!p) {
                cyanrip_log(ctx, 0, "Missing pregap action\n");
                return 1;
            }
            if (!strncmp(p, "default", strlen("default"))) {
                act = CYANRIP_PREGAP_DEFAULT;
            } else if (!strncmp(p, "drop", strlen("drop"))) {
                act = CYANRIP_PREGAP_DROP;
            } else if (!strncmp(p, "merge", strlen("merge"))) {
                act = CYANRIP_PREGAP_MERGE;
            } else if (!strncmp(p, "track", strlen("track"))) {
                act = CYANRIP_PREGAP_TRACK;
            } else {
                cyanrip_log(ctx, 0, "Invalid pregap action %s\n", p);
                return 1;
            }
            settings.pregap_action[idx - 1] = act;
            break;
        case 'c':
            cover_image_path = optarg;
            break;
        case 'E':
            settings.eject_on_success_rip = 1;
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

    if (fill_metadata(ctx, !!album_metadata_ptr || track_metadata_ptr_cnt))
        return 1;

    if (cover_image_path)
        av_dict_set(&ctx->meta, "cover_art", cover_image_path, 0);

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

    if (ctx->settings.base_dst_folder)
        ctx->base_dst_folder = av_strdup(ctx->settings.base_dst_folder);
    else if (dict_get(ctx->meta, "album"))
        ctx->base_dst_folder = cyanrip_sanitize_fn(dict_get(ctx->meta, "album"));
    else if (dict_get(ctx->meta, "discid"))
        ctx->base_dst_folder = cyanrip_sanitize_fn(dict_get(ctx->meta, "discid"));
    else
        ctx->base_dst_folder = av_strdup("Untitled CD");

    if (!ctx->settings.print_info_only)
        cyanrip_log_init(ctx);

    cyanrip_log_start_report(ctx);
    setup_track_offsets_and_report(ctx);

    copy_album_to_track_meta(ctx);

    /* Read user track metadata */
    for (int i = 0; i < track_metadata_ptr_cnt; i++) {
        if (!track_metadata_ptr[i])
            continue;

        char *end = NULL;
        int track_num = strtol(track_metadata_ptr[i], &end, 10) - 1;
        if (track_num < 0 || track_num >= ctx->nb_tracks) {
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

    cyanrip_log(ctx, 0, "Tracks:\n");
    if (ctx->settings.print_info_only) {
        for (int i = 0; i < ctx->nb_tracks; i++) {
            cyanrip_track *t = &ctx->tracks[i];
            cyanrip_log(ctx, 0, "Track %i info:\n", t->number);
            track_read_extra(ctx, t);
            av_dict_set(&t->meta, "cover_art", NULL, 0);
            cyanrip_log_track_end(ctx, t);

            if (cdio_get_media_changed(ctx->cdio)) {
                cyanrip_log(ctx, 0, "Drive media changed, stopping!\n");
                break;
            }

            if (quit_now)
                break;
        }
    } else if (ctx->settings.rip_indices_count == -1) {
        for (int i = 0; i < ctx->nb_tracks; i++) {
            if (cyanrip_rip_track(ctx, &ctx->tracks[i]))
                break;
            if (quit_now)
                break;
        }
    } else {
        for (int i = 0; i < ctx->settings.rip_indices_count; i++) {
            int index = ctx->settings.rip_indices[i] - 1;
            if (index < 0 || index >= ctx->nb_tracks) {
                cyanrip_log(ctx, 0, "Invalid rip index %i, disc has %i tracks!\n",
                            index + 1, ctx->nb_tracks);
                ctx->total_error_count++;
                goto end;
            }
        }
        for (int i = 0; i < ctx->settings.rip_indices_count; i++) {
            int index = ctx->settings.rip_indices[i] - 1;
            if (cyanrip_rip_track(ctx, &ctx->tracks[index]))
                break;
            if (quit_now)
                break;
        }
    }

    if (!ctx->settings.print_info_only)
        cyanrip_log_finish_report(ctx);
end:
    cyanrip_log_end(ctx);

    int err_cnt = ctx->total_error_count;

    cyanrip_ctx_end(&ctx);

    return !!err_cnt;
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
