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
#include <sys/stat.h>

#include <libavutil/bprint.h>
#include <libavutil/avstring.h>

#include "cyanrip_main.h"
#include "cyanrip_log.h"
#include "cyanrip_crc.h"
#include "discid.h"
#include "musicbrainz.h"
#include "accurip.h"
#include "cyanrip_encode.h"
#include "os_compat.h"

int quit_now = 0;

const cyanrip_out_fmt crip_fmt_info[] = {
    [CYANRIP_FORMAT_FLAC]     = { "flac",     "FLAC", "flac",  "flac",  1, 11, 1, AV_CODEC_ID_FLAC,      },
    [CYANRIP_FORMAT_MP3]      = { "mp3",      "MP3",  "mp3",   "mp3",   1,  0, 0, AV_CODEC_ID_MP3,       },
    [CYANRIP_FORMAT_TTA]      = { "tta",      "TTA",  "tta",   "tta",   0,  0, 1, AV_CODEC_ID_TTA,       },
    [CYANRIP_FORMAT_OPUS]     = { "opus",     "OPUS", "opus",  "ogg",   0, 10, 0, AV_CODEC_ID_OPUS,      },
    [CYANRIP_FORMAT_AAC]      = { "aac",      "AAC",  "m4a",   "adts",  0,  0, 0, AV_CODEC_ID_AAC,       },
    [CYANRIP_FORMAT_AAC_MP4]  = { "aac_mp4",  "AAC",  "mp4",   "mp4",   1,  0, 0, AV_CODEC_ID_AAC,       },
    [CYANRIP_FORMAT_WAVPACK]  = { "wavpack",  "WV",   "wv",    "wv",    0,  8, 1, AV_CODEC_ID_WAVPACK,   },
    [CYANRIP_FORMAT_VORBIS]   = { "vorbis",   "OGG",  "ogg",   "ogg",   0,  0, 0, AV_CODEC_ID_VORBIS,    },
    [CYANRIP_FORMAT_ALAC]     = { "alac",     "ALAC", "m4a",   "ipod",  0,  2, 1, AV_CODEC_ID_ALAC,      },
    [CYANRIP_FORMAT_WAV]      = { "wav",      "WAV",  "wav",   "wav",   0,  0, 1, AV_CODEC_ID_NONE,      },
    [CYANRIP_FORMAT_OPUS_MP4] = { "opus_mp4", "OPUS", "mp4",   "mp4",   1, 10, 0, AV_CODEC_ID_OPUS,      },
    [CYANRIP_FORMAT_PCM]      = { "pcm",      "PCM",  "pcm",   "s16le", 0,  0, 1, AV_CODEC_ID_NONE,      },
};

static void cyanrip_ctx_end(cyanrip_ctx **s)
{
    cyanrip_ctx *ctx;
    if (!s || !*s)
        return;
    ctx = *s;

    for (int i = 0; i < ctx->nb_tracks; i++) {
        av_dict_free(&ctx->tracks[i].meta);
        av_free(ctx->tracks[i].ar_db_entries);
    }

    av_free(ctx->mb_submission_url);

    if (ctx->paranoia)
        cdio_paranoia_free(ctx->paranoia);
    if (ctx->drive)
        cdio_cddap_close_no_free_cdio(ctx->drive);
    if (ctx->settings.eject_on_success_rip && !ctx->total_error_count &&
        (ctx->mcap & CDIO_DRIVE_CAP_MISC_EJECT) && ctx->cdio && !quit_now)
        cdio_eject_media(&ctx->cdio);
    else if (ctx->cdio)
        cdio_destroy(ctx->cdio);

    free(ctx->settings.dev_path);
    av_dict_free(&ctx->meta);
    av_freep(&ctx->base_dst_folder);
    av_freep(&ctx);

    *s = NULL;
}

static const paranoia_mode_t paranoia_level_map[] = {
    [0] = PARANOIA_MODE_DISABLE, /* Disable everything */
    [1] = PARANOIA_MODE_OVERLAP, /* Perform overlapped reads */
    [2] = PARANOIA_MODE_OVERLAP | PARANOIA_MODE_VERIFY, /* Perform and verify overlapped reads */
    [3] = PARANOIA_MODE_FULL ^ PARANOIA_MODE_NEVERSKIP, /* Maximum, but do allow skipping sectors */
};
const int crip_max_paranoia_level = (sizeof(paranoia_level_map) / sizeof(paranoia_level_map[0])) - 1;

static int cyanrip_ctx_init(cyanrip_ctx **s, cyanrip_settings *settings)
{
    cyanrip_ctx *ctx = av_mallocz(sizeof(cyanrip_ctx));

    memcpy(&ctx->settings, settings, sizeof(cyanrip_settings));

    if (ctx->settings.print_info_only)
        ctx->settings.eject_on_success_rip = 0;

    cdio_init();

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

    ctx->paranoia = cdio_paranoia_init(ctx->drive);
    if (!ctx->paranoia) {
        cyanrip_log(ctx, 0, "Unable to init paranoia!\n");
        cyanrip_ctx_end(&ctx);
        return AVERROR(EINVAL);
    }

    cdio_paranoia_modeset(ctx->paranoia, paranoia_level_map[settings->paranoia_level]);

    ctx->start_lsn = 0;

    ctx->end_lsn = cdio_get_track_lsn(ctx->cdio, CDIO_CDROM_LEADOUT_TRACK) - 1;
    ctx->duration_frames = ctx->end_lsn - ctx->start_lsn + 1;

    ctx->nb_tracks = ctx->nb_cd_tracks = cdio_cddap_tracks(ctx->drive);
    if ((ctx->nb_tracks < 1) || (ctx->nb_tracks > CDIO_CD_MAX_TRACKS)) {
        cyanrip_log(ctx, 0, "Invalid number of tracks: %i!\n", ctx->nb_tracks);
        ctx->nb_tracks = 0;
        cyanrip_ctx_end(&ctx);
        return AVERROR(EINVAL);
    }

    int first_track_nb = cdio_get_first_track_num(ctx->cdio);
    for (int i = 0; i < ctx->nb_cd_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];

        t->number = t->cd_track_number = i + first_track_nb;
        t->track_is_data = !cdio_cddap_track_audiop(ctx->drive, t->number);
        t->pregap_lsn = cdio_get_track_pregap_lsn(ctx->cdio, t->number);
        t->start_lsn = cdio_get_track_lsn(ctx->cdio, t->number);
        t->end_lsn = cdio_get_track_last_lsn(ctx->cdio, t->number);

        t->start_lsn_sig = t->start_lsn;
        t->end_lsn_sig = t->end_lsn;

        if (t->track_is_data) {
            if (ctx->settings.pregap_action[t->number - 1] == CYANRIP_PREGAP_DEFAULT)
                ctx->settings.pregap_action[t->number - 1] = CYANRIP_PREGAP_DROP;
            if (ctx->settings.pregap_action[t->number - 0] == CYANRIP_PREGAP_DEFAULT)
                ctx->settings.pregap_action[t->number - 0] = CYANRIP_PREGAP_DROP;
            if (i == (ctx->nb_cd_tracks - 1)) {
                ctx->tracks[i - 1].end_lsn -= 11400;
                t->pregap_lsn = ctx->tracks[i - 1].end_lsn + 1;
            }
        }
    }

    for (int i = 0; i < ctx->nb_cd_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];
        if (t->track_is_data)
            continue;
        t->acurip_track_is_first = 1;
        break;
    }

    for (int i = ctx->nb_cd_tracks - 1; i >= 0; i--) {
        cyanrip_track *t = &ctx->tracks[i];
        if (t->track_is_data)
            continue;
        t->acurip_track_is_last = 1;
        break;
    }

    /* For hot removal detection - init this so we can detect changes */
    cdio_get_media_changed(ctx->cdio);

    *s = ctx;
    return 0;
}

static void crip_fill_mcn(cyanrip_ctx *ctx)
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
}

static void track_set_creation_time(cyanrip_ctx *ctx, cyanrip_track *t)
{
    if (dict_get(t->meta, "creation_time"))
        return;

    char t_s[64];
    time_t t_c = time(NULL);
    struct tm *t_l = localtime(&t_c);
    strftime(t_s, sizeof(t_s), "%Y-%m-%dT%H:%M:%S", t_l);
    av_dict_set(&t->meta, "creation_time", t_s, 0);
}

static void copy_album_to_track_meta(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->nb_tracks; i++) {
        av_dict_set(&ctx->tracks[i].meta, "comment", "cyanrip "PROJECT_VERSION_STRING, 0);
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

static const uint8_t *cyanrip_read_frame(cyanrip_ctx *ctx)
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

static int search_for_offset(cyanrip_track *t, int *offset_found,
                             const uint8_t *mem, int dir,
                             int guess, int bytes)
{
    if (guess) {
        const uint8_t *start_addr = mem + guess*4;
        uint32_t accurip_v1 = 0x0;
        for (int j = 0; j < (CDIO_CD_FRAMESIZE_RAW >> 2); j++)
            accurip_v1 += AV_RL32(&start_addr[j*4]) * (j + 1);
        if (crip_find_ar(t, accurip_v1, 1) == t->ar_db_max_confidence && accurip_v1) {
            *offset_found = guess;
            return 1;
        }
    }

    for (int byte_off = ((dir < 0) * 4); byte_off < bytes; byte_off += 4) {
        if (quit_now)
            return 0;

        int offset = dir * (byte_off >> 2);
        if (guess == offset)
            continue;

        const uint8_t *start_addr = mem + dir * byte_off;
        uint32_t accurip_v1 = 0x0;
        for (int j = 0; j < (CDIO_CD_FRAMESIZE_RAW >> 2); j++)
            accurip_v1 += AV_RL32(&start_addr[j*4]) * (j + 1);
        if (crip_find_ar(t, accurip_v1, 1) == t->ar_db_max_confidence && accurip_v1) {
            *offset_found = offset;
            return 1;
        }
    }

    return 0;
}

static void search_for_drive_offset(cyanrip_ctx *ctx, int range)
{
    int had_ar = 0, did_check = 0;
    int offset_found = 0, offset_found_samples = 0;
    uint8_t *mem = av_malloc(2 * range * CDIO_CD_FRAMESIZE_RAW);

    if (ctx->ar_db_status != CYANRIP_ACCUDB_FOUND)
        goto end;

    for (int t_idx = 0; t_idx < ctx->nb_tracks; t_idx++) {
        cyanrip_track *t = &ctx->tracks[t_idx];
        lsn_t start = cdio_get_track_lsn(ctx->cdio, t_idx + 1);
        lsn_t end = cdio_get_track_last_lsn(ctx->cdio, t_idx + 1);

        if (ctx->tracks[t_idx].ar_db_status != CYANRIP_ACCUDB_FOUND)
            continue;
        else
            had_ar |= 1;

        if ((end - start) < (450 + range))
            continue;

        did_check |= 1;

        /* Set start/end ranges */
        start += 450 - range;
        end = start + 2*range;

        size_t bytes = 0;

        cyanrip_log(ctx, 0, "Loading data for track %i...\n", t_idx + 1);
        cdio_paranoia_seek(ctx->paranoia, start, SEEK_SET);
        for (int i = 0; i < 2*range; i++) {
            const uint8_t *data = cyanrip_read_frame(ctx);
            memcpy(mem + bytes, data, CDIO_CD_FRAMESIZE_RAW);
            bytes += CDIO_CD_FRAMESIZE_RAW;
            if (quit_now) {
                cyanrip_log(ctx, 0, "Stopping, offset finding incomplete!\n");
                goto end;
            }
        }

        int found, offset;
        int dir = (offset_found && (offset_found_samples < 0)) ? -1 : +1;

        cyanrip_log(ctx, 0, "Data loaded, searching for offsets...\n");

        found = search_for_offset(t, &offset, mem + (bytes >> 1), dir, offset_found_samples,
                                  range * CDIO_CD_FRAMESIZE_RAW);
        if (!found)
            found = search_for_offset(t, &offset, mem + (bytes >> 1), -dir, 0,
                                      range * CDIO_CD_FRAMESIZE_RAW);

        if (!found) {
            cyanrip_log(ctx, 0, "Nothing found for track %i%s\n", t_idx + 1,
                        t_idx != (ctx->nb_tracks - 1) ? ", trying another track" : "");
        } else if (!offset_found) {
            offset_found_samples = offset;
            offset_found++;
            cyanrip_log(ctx, 0, "Offset of %c%i found in track %i%s\n",
                        offset >= 0 ? '+' : '-', abs(offset), t_idx + 1,
                        t_idx != (ctx->nb_tracks - 1) ? ", trying to confirm with another track" : "");
        } else if (offset_found_samples == offset) {
            offset_found++;
            cyanrip_log(ctx, 0, "Offset of %c%i confirmed (confidence: %i) in track %i%s\n",
                        offset >= 0 ? '+' : '-', abs(offset), offset_found, t_idx + 1,
                        t_idx != (ctx->nb_tracks - 1) ? ", trying to confirm with another track" : "");
        } else {
            cyanrip_log(ctx, 0, "New offset of %c%i found at track %i, scrapping old offset of %c%i%s\n",
                        offset >= 0 ? '+' : '-', abs(offset), t_idx + 1,
                        offset_found_samples >= 0 ? '+' : '-', abs(offset_found_samples),
                        t_idx != (ctx->nb_tracks - 1) ? ", trying to confirm with another track" : "");
            offset_found_samples = offset;
            offset_found = 1;
        }
    }

end:
    av_free(mem);

    if (!offset_found) {
        if (!had_ar) {
            cyanrip_log(ctx, 0, "No track had AccuRip entry, cannot find offset!\n");
        } else if (had_ar && !did_check) {
            cyanrip_log(ctx, 0, "No track was long enough, unable to find drive offset!\n");
        } else {
            cyanrip_log(ctx, 0, "Was not able to find drive offset with a radius of %i frames"
                        ", trying again with a larger radius...\n", range);
            search_for_drive_offset(ctx, 2*range);
        }
        return;
    } else {
        cyanrip_log(ctx, 0, "Drive offset of %c%i found (confidence: %i)!\n",
                    offset_found_samples >= 0 ? '+' : '-', abs(offset_found_samples), offset_found);
    }
}

static void track_read_extra(cyanrip_ctx *ctx, cyanrip_track *t)
{
    if (!t->track_is_data) {
        t->preemphasis = cdio_cddap_track_preemp(ctx->drive, t->cd_track_number);

        if (ctx->rcap & CDIO_DRIVE_CAP_READ_ISRC && !dict_get(t->meta, "isrc")) {
            const char *isrc_str = cdio_get_track_isrc(ctx->cdio, t->cd_track_number);
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
        cyanrip_log(ctx, 0, "Track %i is data:\n", t->number);
        cyanrip_log_track_end(ctx, t);
        return 0;
    }

    /* Hopefully reduce seeking by reading this here */
    track_read_extra(ctx, t);

    cdio_paranoia_seek(ctx->paranoia, t->start_lsn, SEEK_SET);

    /* Set creation time at the start of ripping */
    track_set_creation_time(ctx, t);

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

        process_checksums(&crc_ctx, data, bytes);
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

        /* Flush paranoia cache if overreading into lead-out - no idea why */
        if ((t->start_lsn + i) > ctx->end_lsn)
            cdio_paranoia_seek(ctx->paranoia, t->start_lsn + i, SEEK_SET);

        int bytes = CDIO_CD_FRAMESIZE_RAW;
        const uint8_t *data = cyanrip_read_frame(ctx);

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
        process_checksums(&crc_ctx, data, bytes);

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

        process_checksums(&crc_ctx, data, bytes);
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
            cyanrip_log(ctx, 0, "splitting off into a new track, number %i\n", ct->number > 1 ? ct->number : 0);

            if (lt)
                lt->end_lsn = ct->pregap_lsn - 1;

            if (ct->number > 1) /* Push all track numbers up if needed */
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
            nt->cd_track_number = ct->cd_track_number;

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
    if (ctx->end_lsn > lt->end_lsn && !lt->track_is_data) {
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

/* Key 1 and 2 must be set, and src will be modified */
static char *append_missing_keys(char *src, const char *key1, const char *key2)
{
    /* Copy string with enough space to append extra */
    char *copy = av_mallocz(strlen(src) + strlen(key1) + strlen(key2) + 1);
    memcpy(copy, src, strlen(src));

    int add_key1_offset = -1;
    int add_key2_offset = -1;

    /* Look for keyless entries */
    int count = 0;
    char *p_save, *p = av_strtok(src, ":", &p_save);
    while (p) {
        if (!strstr(p, "=")) {
            if (count == 0)
                add_key1_offset = p - src;
            else if (count == 1)
                add_key2_offset = p - src;
        }
        p = av_strtok(NULL, ":", &p_save);
        if (++count >= 2)
            break;
    }

    /* Prepend key1 if missing */
    if (add_key1_offset >= 0) {
        memmove(&copy[add_key1_offset + strlen(key1)], &copy[add_key1_offset], strlen(copy) - add_key1_offset);
        memcpy(&copy[add_key1_offset], key1, strlen(key1));
        if (add_key2_offset >= 0)
            add_key2_offset += strlen(key1);
    }

    /* Prepend key2 if missing */
    if (add_key2_offset >= 0) {
        memmove(&copy[add_key2_offset + strlen(key2)], &copy[add_key2_offset], strlen(copy) - add_key2_offset);
        memcpy(&copy[add_key2_offset], key2, strlen(key2));
    }

    return copy;
}

static inline int crip_is_integer(const char *src)
{
    for (int i = 0; i < strlen(src); i++)
        if (!av_isdigit(src[i]))
            return 0;
    return 1;
}

struct CRIPCharReplacement {
    const char from;
    const char to;
    const char to_u[5];
    size_t len_u;
    int is_avail_locally;
} crip_char_replacement[] = {
    { '<', '_', "‹", strlen("‹"), HAS_CH_LESS },
    { '>', '_', "›", strlen("›"), HAS_CH_MORE },
    { ':', '_', "∶", strlen("∶"), HAS_CH_COLUMN },
    { '|', '_', "│", strlen("│"), HAS_CH_OR },
    { '?', '_', "？", strlen("？"), HAS_CH_Q },
    { '*', '_', "∗", strlen("∗"), HAS_CH_ANY },
    { '/', '_', "∕", strlen("∕"), HAS_CH_FWDSLASH },
    { '\'', '_', "⧹", strlen("⧹"), HAS_CH_BWDSLASH },
    { '"', '\'', "“", strlen("“"), HAS_CH_QUOTES },
    { '"', '\'', "”", strlen("”"), HAS_CH_QUOTES },
    { 0 },
};

static inline char *crip_sanitize_str(cyanrip_ctx *ctx, char *src_str)
{
    int32_t cp, ret, quote_match = 0;
    char *str_start = av_mallocz(4*strlen(src_str) + 1); /* Worst case */
    char *end = str_start + strlen(src_str), *str = str_start, *pos = str_start;

    memcpy(str_start, src_str, strlen(src_str));

    while (str < end) {
        ret = av_utf8_decode(&cp, (const uint8_t **)&str, end, AV_UTF8_FLAG_ACCEPT_ALL);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error parsing string: %s!\n", av_err2str(ret));
            return str_start;
        }

        struct CRIPCharReplacement *rep = NULL;
        for (int i = 0; crip_char_replacement[i].from; i++) {
            if (cp == crip_char_replacement[i].from) {
                int is_quote = crip_char_replacement[i].from == '"';
                rep = &crip_char_replacement[i + (is_quote && quote_match)];
                quote_match = (quote_match + 1) & 1;
                break;
            }
        }

        if (!rep) {
            pos = str;
            continue;
        }

        switch (ctx->settings.sanitize_method) {
        case CRIP_SANITIZE_OS_SIMPLE:
            if (rep->is_avail_locally)
                break;
        case CRIP_SANITIZE_SIMPLE:
            *pos = rep->to;
            break;
        case CRIP_SANITIZE_OS_UNICODE:
            if (rep->is_avail_locally)
                break;
        case CRIP_SANITIZE_UNICODE:
            memmove(pos + rep->len_u, pos + 1, end - pos - 1);
            memcpy(pos, rep->to_u, rep->len_u);
            str += rep->len_u;
            end += rep->len_u;
            break;
        }

        pos = str;
    }

    return str_start;
}

static char *get_dir_tag_val(cyanrip_ctx *ctx, AVDictionary *meta,
                             const char *ofmt, const char *key)
{
    char *val = NULL;
    if (!strcmp(key, "year")) {
        const char *date = dict_get(meta, "date");
        if (date) {
            char *save_year, *date_dup = av_strdup(date);
            val = av_strdup(av_strtok(date_dup, ":-", &save_year));
            av_free(date_dup);
        }
    } else if (!strcmp(key, "format")) {
        val = av_strdup(ofmt);
    } else if (!strcmp(key, "track")) {
        const char *track = dict_get(meta, "track");
        if (crip_is_integer(track)) {
            int pad = 0, digits = strlen(track);
            if (((digits + pad) < 2) && ctx->nb_tracks >  9) pad++;
            if (((digits + pad) < 3) && ctx->nb_tracks > 99) pad++;
            val = av_mallocz(pad + digits + 1);
            for (int i = 0; i < pad; i++)
                val[i] = '0';
            memcpy(&val[pad], track, digits);
        } else {
            val = av_strdup(track);
        }
    } else {
        val = av_strdup(dict_get(meta, key));
    }

    return val;
}

#define BUF_SANITIZE_APPEND(CTX, BUF, STR_IN)         \
    do {                                              \
        char *str = crip_sanitize_str(CTX, STR_IN);   \
        av_bprint_append_data(BUF, str, strlen(str)); \
        av_free(str);                                 \
    } while (0)

static int process_cond(cyanrip_ctx *ctx, AVBPrint *buf, AVDictionary *meta,
                        const char *ofmt, const char *scheme)
{
    char *scheme_copy = av_strdup(scheme);

    char *save, *tok = av_strtok(scheme_copy, "{}", &save);
    while (tok) {
        if (((tok > scheme_copy) && (tok[-1] == '\''))) {
            BUF_SANITIZE_APPEND(ctx, buf, tok);
            tok = av_strtok(NULL, "{}", &save);
            continue;
        }

        if (!strncmp(tok, "if", strlen("if"))) {
            char *cond = av_strdup(tok);
            char *cond_save, *cond_tok = av_strtok(cond, "#", &cond_save);

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no \"#\"!\n");
                av_free(cond);
                goto fail;
            }

            char *val1 = get_dir_tag_val(ctx, meta, ofmt, cond_tok);
            if (!val1)
                val1 = av_strdup(tok);

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no terminating \"#\"!\n");
                av_free(cond);
                av_free(val1);
                goto fail;
            }

            int cond_is_eq = 0, cond_is_not_eq = 0, cond_is_more = 0, cond_is_less = 0;
            if (strstr(cond_tok, "==")) {
                cond_is_eq = 1;
            } else if (strstr(cond_tok, "!=")) {
                cond_is_not_eq = 1;
            } else if (strstr(cond_tok, ">")) {
                cond_is_more = 1;
            } else if (strstr(cond_tok, "<")) {
                cond_is_less = 1;
            } else {
                cyanrip_log(ctx, 0, "Invalid condition syntax!\n");
                av_free(cond);
                av_free(val1);
                goto fail;
            }

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no terminating \"#\"!\n");
                goto fail;
            }

            char *val2 = get_dir_tag_val(ctx, meta, ofmt, cond_tok);
            if (!val2)
                val2 = av_strdup(cond_tok);

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no terminating \"#\"!\n");
                goto fail;
            }

            int cond_true = 0;
            cond_true |= cond_is_eq && !strcmp(val1, val2);
            cond_true |= cond_is_not_eq && strcmp(val1, val2);

            if (cond_is_less || cond_is_more) {
                int64_t val1_dec = crip_is_integer(val1) ? strtol(val1, NULL, 10) : (cond_is_more ? INT64_MIN : INT64_MAX);
                int64_t val2_dec = crip_is_integer(val1) ? strtol(val2, NULL, 10) : (cond_is_less ? INT64_MIN : INT64_MAX);
                cond_true |= cond_is_less && val1_dec < val2_dec;
                cond_true |= cond_is_more && val1_dec > val2_dec;
            }

            if (cond_true) {
                char *true_save, *true_tok = av_strtok(cond_tok, "|", &true_save);
                while (true_tok) {
                    if (((true_tok > cond_tok) && (true_tok[-1] == '\''))) {
                        BUF_SANITIZE_APPEND(ctx, buf, true_tok);
                        true_tok = av_strtok(NULL, "|", &true_save);
                        continue;
                    }

                    char *true_val = get_dir_tag_val(ctx, meta, ofmt, true_tok);
                    if (!true_val)
                        true_val = av_strdup(true_tok);

                    BUF_SANITIZE_APPEND(ctx, buf, true_val);
                    av_free(true_val);

                    true_tok = av_strtok(NULL, "|", &true_save);
                }
            }

            av_free(val2);
            av_free(val1);
            av_free(cond);

            tok = av_strtok(NULL, "{}", &save);
            continue;
        }

        char *val = get_dir_tag_val(ctx, meta, ofmt, tok);
        if (!val)
            val = av_strdup(tok);

        BUF_SANITIZE_APPEND(ctx, buf, val);
        av_free(val);

        tok = av_strtok(NULL, "{}", &save);
    }

    av_free(scheme_copy);
    return 0;

fail:
    av_free(scheme_copy);
    return AVERROR(EINVAL);
}

char *crip_get_path(cyanrip_ctx *ctx, enum CRIPPathType type,
                    const cyanrip_out_fmt *fmt, void *arg)
{
    char *ret = NULL, **ret_p = NULL;
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);

    if (process_cond(ctx, &buf, ctx->meta, fmt->folder_suffix, ctx->settings.folder_name_scheme))
        goto end;

    if (type == CRIP_PATH_FOLDER) {
        ret_p = &ret;
        goto end;
    }

    av_bprintf(&buf, "/");

    char *ext = NULL;
    if (type == CRIP_PATH_COVERART) {
        BUF_SANITIZE_APPEND(ctx, &buf, arg);
        CRIPArt *art = arg;
        ext = av_strdup(art->extension);
    } else if (type == CRIP_PATH_LOG) {
        if (process_cond(ctx, &buf, ctx->meta, fmt->name, ctx->settings.log_name_scheme))
            goto end;
        ext = av_strdup("log");
    } else {
        cyanrip_track *t = arg;
        if (process_cond(ctx, &buf, t->meta, fmt->name, ctx->settings.track_name_scheme))
            goto end;
        ext = av_strdup(fmt->ext);
    }

    if (ext)
        av_bprintf(&buf, ".%s", ext);
    av_free(ext);

    ret_p = &ret;

end:
    av_bprint_finalize(&buf, ret_p);
    return ret;
}

int main(int argc, char **argv)
{
    cyanrip_ctx *ctx = NULL;
    cyanrip_settings settings;

    if (signal(SIGINT, on_quit_signal) == SIG_ERR)
        cyanrip_log(ctx, 0, "Can't init signal handler!\n");

    /* Default settings */
    settings.dev_path = NULL;
    settings.folder_name_scheme = "{album} [{format}]";
    settings.track_name_scheme = "{if #totaldiscs# > #1#|disc|.}{track} - {title}";
    settings.log_name_scheme = "{album}{if #totaldiscs# > #1# CD|disc|}";
    settings.sanitize_method = CRIP_SANITIZE_UNICODE;
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
    settings.disable_accurip = 0;
    settings.eject_on_success_rip = 0;
    settings.outputs[0] = CYANRIP_FORMAT_FLAC;
    settings.outputs_num = 1;
    settings.paranoia_level = FF_ARRAY_ELEMS(paranoia_level_map) - 1;

    memset(settings.pregap_action, CYANRIP_PREGAP_DEFAULT, sizeof(settings.pregap_action));

    int c;
    char *p_save, *p;
    int mb_release_idx = -1;
    char *mb_release_str = NULL;
    int discnumber = 0, totaldiscs = 0;
    char *cover_image_path = NULL;
    char *album_metadata_ptr = NULL;
    char *track_metadata_ptr[99] = { NULL };
    int track_metadata_ptr_cnt = 0;
    int find_drive_offset_range = 0;

    while ((c = getopt(argc, argv, "hnfAHIVEOl:a:t:b:c:r:d:o:s:S:D:p:C:R:P:F:L:T:")) != -1) {
        switch (c) {
        case 'h':
            cyanrip_log(ctx, 0, "cyanrip %s (%s) help:\n", PROJECT_VERSION_STRING, vcstag);
            cyanrip_log(ctx, 0, "\n  Ripping options:\n");
            cyanrip_log(ctx, 0, "    -d <path>             Set device path\n");
            cyanrip_log(ctx, 0, "    -s <int>              CD Drive offset in samples (default: 0)\n");
            cyanrip_log(ctx, 0, "    -r <int>              Maximum number of retries to read a frame (default: 25)\n");
            cyanrip_log(ctx, 0, "    -S <int>              Set drive speed (default: unset)\n");
            cyanrip_log(ctx, 0, "    -p <number>=<string>  Track pregap handling (default: default)\n");
            cyanrip_log(ctx, 0, "    -P <int>              Paranoia level, %i to 0 inclusive, default: %i\n", crip_max_paranoia_level, settings.paranoia_level);
            cyanrip_log(ctx, 0, "    -O                    Enable overreading into lead-in and lead-out\n");
            cyanrip_log(ctx, 0, "    -H                    Enable HDCD decoding. Do this if you're sure disc is HDCD\n");
            cyanrip_log(ctx, 0, "\n  Metadata options:\n");
            cyanrip_log(ctx, 0, "    -I                    Only print CD and track info\n");
            cyanrip_log(ctx, 0, "    -a <string>           Album metadata, key=value:key=value\n");
            cyanrip_log(ctx, 0, "    -t <number>=<string>  Track metadata, can be specified multiple times\n");
            cyanrip_log(ctx, 0, "    -R <int>/<string>     Sets the MusicBrainz release to use, either as an index starting from 1 or an ID string\n");
            cyanrip_log(ctx, 0, "    -c <path>             Set cover image path\n");
            cyanrip_log(ctx, 0, "    -n                    Disables MusicBrainz lookup and ignores lack of manual metadata\n");
            cyanrip_log(ctx, 0, "    -A                    Disables AccurateRip database query and validation\n");
            cyanrip_log(ctx, 0, "    -C <int>/<int>        Tag multi-disc albums, syntax is disc/totaldiscs\n");
            cyanrip_log(ctx, 0, "\n  Output options:\n");
            cyanrip_log(ctx, 0, "    -l <list>             Select which tracks to rip (default: all)\n");
            cyanrip_log(ctx, 0, "    -D <string>           Directory naming scheme, by default its \"%s\"\n", settings.folder_name_scheme);
            cyanrip_log(ctx, 0, "    -F <string>           Track naming scheme, by default its \"%s\"\n", settings.track_name_scheme);
            cyanrip_log(ctx, 0, "    -L <string>           Log file name scheme, by default its \"%s\"\n", settings.log_name_scheme);
            cyanrip_log(ctx, 0, "    -T <string>           Filename sanitation: simple, os_simple, unicode (default), os_unicode\n");
            cyanrip_log(ctx, 0, "    -o <string>           Comma separated list of outputs\n");
            cyanrip_log(ctx, 0, "    -b <kbps>             Bitrate of lossy files in kbps\n");
            cyanrip_log(ctx, 0, "\n  Misc. options:\n");
            cyanrip_log(ctx, 0, "    -E                    Eject tray once successfully done\n");
            cyanrip_log(ctx, 0, "    -V                    Print program version\n");
            cyanrip_log(ctx, 0, "    -h                    Print options help\n");
            cyanrip_log(ctx, 0, "    -f                    Find drive offset (requires a disc with an AccuRip DB entry)\n");
            return 0;
            break;
        case 'S':
            settings.speed = (int)strtol(optarg, NULL, 10);
            if (settings.speed < 0) {
                cyanrip_log(ctx, 0, "Invalid drive speed!\n");
                return 1;
            }
            break;
        case 'P':
            if (!strcmp(optarg, "none"))
                settings.paranoia_level = 0;
            else if (!strcmp(optarg, "max"))
                settings.paranoia_level = crip_max_paranoia_level;
            else
                settings.paranoia_level = (int)strtol(optarg, NULL, 10);
            if (settings.paranoia_level < 0 || settings.paranoia_level > crip_max_paranoia_level) {
                cyanrip_log(ctx, 0, "Invalid paranoia level %i must be between 0 and %i!\n",
                            settings.paranoia_level, crip_max_paranoia_level);
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
        case 'R':
            p = NULL;
            mb_release_idx = strtol(optarg, &p, 10);
            if (p != NULL && p[0] != ' ' && p[0] != '\0') {
                mb_release_str = optarg;
                mb_release_idx = -1;
            } else if (mb_release_idx <= 0) {
                cyanrip_log(ctx, 0, "Invalid release index %i!\n", mb_release_idx);
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
            p = av_strtok(optarg, ",", &p_save);
            while (p) {
                int idx = strtol(p, NULL, 10);
                for (int i = 0; i < settings.rip_indices_count; i++) {
                    if (settings.rip_indices[i] == idx) {
                        cyanrip_log(ctx, 0, "Duplicated rip idx %i\n", idx);
                        return 1;
                    }
                }
                settings.rip_indices[settings.rip_indices_count++] = idx;
                p = av_strtok(NULL, ",", &p_save);
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
            p = av_strtok(optarg, ",", &p_save);
            while (p) {
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
                p = av_strtok(NULL, ",", &p_save);
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
        case 'A':
            settings.disable_accurip = 1;
            break;
        case 'f':
            find_drive_offset_range = 6;
            break;
        case 'C':
            p = av_strtok(optarg, "/", &p_save);
            discnumber = strtol(p, NULL, 10);
            if (discnumber <= 0) {
                cyanrip_log(ctx, 0, "Invalid discnumber %i\n", discnumber);
                return 1;
            }
            p = av_strtok(NULL, "/", &p_save);
            if (!p)
                break;
            totaldiscs = strtol(p, NULL, 10);
            if (totaldiscs <= 0) {
                cyanrip_log(ctx, 0, "Invalid totaldiscs %i\n", totaldiscs);
                return 1;
            }
            if (discnumber > totaldiscs) {
                cyanrip_log(ctx, 0, "discnumber %i is larger than totaldiscs %i\n", discnumber, totaldiscs);
                return 1;
            }
            break;
        case 'p':
            p = av_strtok(optarg, "=", &p_save);
            int idx = strtol(p, NULL, 10);
            if (idx < 1 || idx > 99) {
                cyanrip_log(ctx, 0, "Invalid track idx %i\n", idx);
                return 1;
            }
            enum cyanrip_pregap_action act = CYANRIP_PREGAP_DEFAULT;
            p = av_strtok(NULL, "=", &p_save);
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
            settings.folder_name_scheme = optarg;
            break;
        case 'F':
            settings.track_name_scheme = optarg;
            break;
        case 'L':
            settings.log_name_scheme = optarg;
            break;
        case 'T':
            if (!strncmp(optarg, "simple", strlen("simple"))) {
                settings.sanitize_method = CRIP_SANITIZE_SIMPLE;
            } else if (!strncmp(optarg, "os_simple", strlen("os_simple"))) {
                settings.sanitize_method = CRIP_SANITIZE_OS_SIMPLE;
            } else if (!strncmp(optarg, "unicode", strlen("unicode"))) {
                settings.sanitize_method = CRIP_SANITIZE_UNICODE;
            } else if (!strncmp(optarg, "os_unicode", strlen("os_unicode"))) {
                settings.sanitize_method = CRIP_SANITIZE_OS_UNICODE;
            } else {
                cyanrip_log(ctx, 0, "Invalid sanitation method %s\n", optarg);
                return 1;
            }
            break;
        case 'V':
            cyanrip_log(ctx, 0, "cyanrip %s (%s)\n", PROJECT_VERSION_STRING, vcstag);
            return 0;
        case 'd':
            settings.dev_path = strdup(optarg);
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

    if (settings.outputs_num > 1 && !strstr(settings.folder_name_scheme, "$format$")) {
        cyanrip_log(ctx, 0, "Directory name scheme must contain $format$ with multiple output formats!\n");
        return 1;
    }

    if (find_drive_offset_range) {
        settings.disable_accurip = 0;
        settings.disable_mb = 1;
        settings.offset = 0;
        settings.eject_on_success_rip = 0;
        cyanrip_log(ctx, 0, "Searching for drive offset, enabling AccuRip and disabling MusicBrainz\n");
    }

    if (cyanrip_ctx_init(&ctx, &settings))
        return 1;

    /* Fill disc MCN */
    crip_fill_mcn(ctx);

    /* Fill discid */
    if (crip_fill_discid(ctx)) {
        ctx->total_error_count++;
        goto end;
    }

    /* Fill musicbrainz metadata */
    if (crip_fill_metadata(ctx,
                           !!album_metadata_ptr || track_metadata_ptr_cnt,
                           mb_release_idx, mb_release_str, discnumber)) {
        ctx->total_error_count++;
        goto end;
    }

    /* Print this for easy access */
    if (ctx->settings.print_info_only)
        cyanrip_log(ctx, 0, "MusicBrainz URL:\n%s\n", ctx->mb_submission_url);

    /* Fill in accurip data */
    if (crip_fill_accurip(ctx)) {
        ctx->total_error_count++;
        goto end;
    }

    if (find_drive_offset_range) {
        search_for_drive_offset(ctx, find_drive_offset_range);
        goto end;
    }

    if (mb_release_str && !dict_get(ctx->meta, "release_id"))
        av_dict_set(&ctx->meta, "release_id", mb_release_str, 0);

    if (cover_image_path)
        av_dict_set(&ctx->meta, "cover_art", cover_image_path, 0);

    if (discnumber)
        av_dict_set_int(&ctx->meta, "disc", discnumber, 0);

    if (totaldiscs)
        av_dict_set_int(&ctx->meta, "totaldiscs", totaldiscs, 0);

    /* Read user album metadata */
    if (album_metadata_ptr) {
        /* Fixup */
        char *copy = append_missing_keys(album_metadata_ptr, "album=", "album_artist=");

        /* Parse */
        int err = av_dict_parse_string(&ctx->meta, copy, "=", ":", 0);
        av_free(copy);
        if (err) {
            cyanrip_log(ctx, 0, "Error reading album tags: %s\n",
                        av_err2str(err));
            ctx->total_error_count++;
            goto end;
        }

        /* Fixup title tag mistake */
        const char *title = dict_get(ctx->meta, "title");
        const char *album = dict_get(ctx->meta, "album");
        if (title && !album) {
            av_dict_set(&ctx->meta, "album", title, 0);
            av_dict_set(&ctx->meta, "title", "", 0);
        }

        /* Populate artist tag if missing/unspecified */
        const char *album_artist = dict_get(ctx->meta, "album_artist");
        const char *artist = dict_get(ctx->meta, "artist");
        if (album_artist && !artist)
            av_dict_set(&ctx->meta, "artist", album_artist, 0);
        else if (artist && !album_artist)
            av_dict_set(&ctx->meta, "album_artist", artist, 0);
    }

    if (!ctx->settings.print_info_only) {
        /* Create directories */
        for (int i = 0; i < ctx->settings.outputs_num; i++) {
            char *dirname = crip_get_path(ctx, CRIP_PATH_FOLDER, &crip_fmt_info[ctx->settings.outputs[i]], NULL);
            struct stat st_req = { 0 };
            if (stat(dirname, &st_req) == -1)
                mkdir(dirname, 0700);
            av_free(dirname);
        }

        /* Create logfile */
        cyanrip_log_init(ctx);
    }

    cyanrip_log_start_report(ctx);
    setup_track_offsets_and_report(ctx);

    copy_album_to_track_meta(ctx);

    /* Read user track metadata */
    for (int i = 0; i < track_metadata_ptr_cnt; i++) {
        if (!track_metadata_ptr[i])
            continue;

        char *end = NULL;
        int u_nb = strtol(track_metadata_ptr[i], &end, 10);

        /* Verify all indices */
        int track_idx = 0;
        for (; track_idx < ctx->nb_tracks; track_idx++) {
            if (ctx->tracks[track_idx].number == u_nb)
                break;
        }
        if (track_idx >= ctx->nb_tracks) {
            cyanrip_log(ctx, 0, "Invalid track number %i, list has %i tracks!\n",
                        u_nb, ctx->nb_tracks);
            ctx->total_error_count++;
            goto end;
        }

        end += 1; /* Move past equal sign */

        /* Fixup */
        char *copy = append_missing_keys(end, "title=", "artist=");

        /* Parse */
        int err = av_dict_parse_string(&ctx->tracks[track_idx].meta,
                                       copy, "=", ":", 0);
        av_free(copy);
        if (err) {
            cyanrip_log(ctx, 0, "Error reading track tags: %s\n",
                        av_err2str(err));
            ctx->total_error_count++;
            goto end;
        }
    }

    for (int i = 0; i < ctx->nb_tracks; i++) {
        for (int f = 0; f < ctx->settings.outputs_num; f++) {
            char *logfile = crip_get_path(ctx, CRIP_PATH_TRACK,
                                          &crip_fmt_info[ctx->settings.outputs[f]],
                                          &ctx->tracks[i]);
            av_free(logfile);
        }
    }

    cyanrip_log(ctx, 0, "Tracks:\n");
    if (ctx->settings.rip_indices_count == -1) {
        for (int i = 0; i < ctx->nb_tracks; i++) {
            cyanrip_track *t = &ctx->tracks[i];
            if (ctx->settings.print_info_only) {
                cyanrip_log(ctx, 0, "Track %i info:\n", t->number);
                track_read_extra(ctx, t);
                av_dict_set(&t->meta, "cover_art", NULL, 0);
                cyanrip_log_track_end(ctx, t);

                if (cdio_get_media_changed(ctx->cdio)) {
                    cyanrip_log(ctx, 0, "Drive media changed, stopping!\n");
                    break;
                }
            } else {
                if (cyanrip_rip_track(ctx, t))
                    break;
            }

            if (quit_now)
                break;
        }
    } else {
        for (int i = 0; i < ctx->settings.rip_indices_count; i++) {
            int idx = ctx->settings.rip_indices[i];

            /* Verify all indices */
            int j = 0;
            for (; j < ctx->nb_tracks; j++) {
                if (ctx->tracks[j].number == idx)
                    break;
            }
            if (j >= ctx->nb_tracks) {
                cyanrip_log(ctx, 0, "Invalid rip index %i, list has %i tracks!\n",
                            idx, ctx->nb_tracks);
                ctx->total_error_count++;
                goto end;
            }
        }

        for (int i = 0; i < ctx->settings.rip_indices_count; i++) {
            int idx = ctx->settings.rip_indices[i];
            int j = 0;
            for (; j < ctx->nb_tracks; j++) {
                if (ctx->tracks[j].number == idx)
                    break;
            }

            if (ctx->settings.print_info_only) {
                cyanrip_log(ctx, 0, "Track %i info:\n", ctx->tracks[j].number);
                track_read_extra(ctx, &ctx->tracks[j]);
                av_dict_set(&ctx->tracks[j].meta, "cover_art", NULL, 0);
                cyanrip_log_track_end(ctx, &ctx->tracks[j]);

                if (cdio_get_media_changed(ctx->cdio)) {
                    cyanrip_log(ctx, 0, "Drive media changed, stopping!\n");
                    break;
                }
            } else {
                if (cyanrip_rip_track(ctx, &ctx->tracks[j]))
                    break;
            }

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
