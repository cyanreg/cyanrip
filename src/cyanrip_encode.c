/*
 * Copyright (C) 2016 Rostislav Pehlivanov <atomnuker@gmail.com>
 *
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
#include <stdarg.h>
#include <sys/stat.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>

#include "cyanrip_encode.h"
#include "cyanrip_log.h"

typedef struct cyanrip_out_fmt {
    const char *name;
    const char *ext;
} cyanrip_out_fmt;

cyanrip_out_fmt fmt_map[] = {
    [CYANRIP_FORMAT_FLAC]    = { "FLAC", "flac" },
    [CYANRIP_FORMAT_TTA]     = { "TTA",  "tta"  },
};

cyanrip_ctx *log_ctx = NULL;

static void log_cb_wrapper(void *avcl, int level, const char *fmt, va_list vl)
{
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    cyanrip_log(log_ctx, 0, "%s", buffer);
}

void cyanrip_init_encoding(cyanrip_ctx *ctx)
{
    av_register_all();
}

void cyanrip_end_encoding(cyanrip_ctx *ctx)
{
    free(ctx->cover_image_pkt);
}

int cyanrip_setup_cover_image(cyanrip_ctx *ctx)
{
    if (!ctx->settings.cover_image_path) {
        ctx->cover_image_pkt = NULL;
        return 0;
    }

    if (LIBAVFORMAT_VERSION_MAJOR < 57 ||
        LIBAVFORMAT_VERSION_MINOR < 72 ||
        LIBAVFORMAT_VERSION_MICRO < 101) {
        cyanrip_log(ctx, 0, "Can't mux cover art, ffmpeg version too old!\n");
        return 1;
    }

    AVFormatContext *avf = NULL;
    if (avformat_open_input(&avf, ctx->settings.cover_image_path, NULL, NULL) < 0) {
        cyanrip_log(ctx, 0, "Unable to open \"%s\"!\n", ctx->settings.cover_image_path);
        goto fail;
    }

    if (avformat_find_stream_info(avf, NULL) < 0) {
        cyanrip_log(ctx, 0, "Unable to get cover image info!\n");
        goto fail;
    }

    ctx->cover_image_codec_id = avf->streams[0]->codecpar->codec_id;

    AVCodec *codec = avcodec_find_decoder(avf->streams[0]->codecpar->codec_id);
    if (!codec) {
        cyanrip_log(ctx, 0, "Unable to find a valid image decoder!\n");
        goto fail;
    }

    AVPacket *pkt = ctx->cover_image_pkt;
    pkt = calloc(1, sizeof(AVPacket));
    av_init_packet(pkt);

    if (av_read_frame(avf, pkt) < 0) {
        cyanrip_log(ctx, 0, "Error demuxing cover image!\n");
        goto fail;
    }

    cyanrip_log(ctx, 0, "Cover image \"%s\" demuxed, codec - (%s)%s!\n",
                ctx->settings.cover_image_path, codec->name, codec->long_name);

    avformat_free_context(avf);

    return 0;

fail:
    return 1;
}

static void set_metadata(cyanrip_ctx *ctx, cyanrip_track *t, AVFormatContext *avf)
{
    char t_s[64];
    char t_disc_date[64];

    time_t t_c = time(NULL);
    struct tm *t_l = localtime(&t_c);
    strftime(t_s, sizeof(t_s), "%FT%H:%M:%S", t_l);

    strftime(t_disc_date, sizeof(t_disc_date), "%FT%H:%M:%S", ctx->disc_date);

    av_dict_set    (&avf->metadata, "comment",            "cyanrip",         0);
    av_dict_set    (&avf->metadata, "title",              t->name,           0);
    av_dict_set_int(&avf->metadata, "track",              t->index + 1,      0);
    av_dict_set    (&avf->metadata, "creation_time",      t_s,               0);
    av_dict_set    (&avf->metadata, "album",              ctx->disc_name,    0);
    av_dict_set    (&avf->metadata, "album_artist",       ctx->album_artist, 0);
    av_dict_set    (&avf->metadata, "date",               t_disc_date,       0);
    av_dict_set    (&avf->metadata, "musicbrainz_discid", ctx->discid,       0);
}

int cyanrip_encode_track(cyanrip_ctx *ctx, cyanrip_track *t,
                         cyanrip_output_settings *settings)
{
    int ret;

    log_ctx = ctx;
    av_log_set_callback(log_cb_wrapper);

    cyanrip_out_fmt *cfmt = &fmt_map[settings->format];

    char dirname[259], filename[1024];
    sprintf(dirname, "%s [%s]", ctx->disc_name, cfmt->name);
    sprintf(filename, "%s/%02i - %s.%s", dirname, t->index + 1, t->name, cfmt->ext);

    struct stat st_req = { 0 };
    if (stat(dirname, &st_req) == -1)
        mkdir(dirname, 0700);

    AVFormatContext *avf = NULL;
    if (avformat_alloc_output_context2(&avf, NULL, cfmt->ext, filename) < 0) {
        cyanrip_log(ctx, 0, "Unable to init lavf context!\n");
        goto fail;
    }
    AVOutputFormat *fmt = avf->oformat;

    AVStream *st = avformat_new_stream(avf, NULL);
    if (!st) {
        cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
        goto fail;
    }

    AVStream *st_img = NULL;
    if (ctx->cover_image_pkt) {
        fmt->video_codec = ctx->cover_image_codec_id;
        st_img = avformat_new_stream(avf, NULL);
        if (!st_img) {
            cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
            goto fail;
        }
    }

    AVCodec *codec = avcodec_find_encoder(fmt->audio_codec);
    if (!codec) {
        cyanrip_log(ctx, 0, "Codec not found!\n");
        goto fail;
    }

    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx) {
        cyanrip_log(ctx, 0, "Unable to init avctx!\n");
        goto fail;
    }

    avctx->opaque         = ctx;
    avctx->bit_rate       = lrintf(settings->bitrate*1000.0f);
    avctx->sample_fmt     = AV_SAMPLE_FMT_S16;
    avctx->channel_layout = AV_CH_LAYOUT_STEREO;
    avctx->sample_rate    = 44100;
    avctx->channels       = 2;
    st->id                = 0;
    if (ctx->cover_image_pkt) {
        st_img->id = 1;
        st_img->disposition |= AV_DISPOSITION_ATTACHED_PIC;
        st_img->nb_frames = 1;
        st_img->time_base = (AVRational){ 1, 25 };
        st_img->codecpar->codec_id = ctx->cover_image_codec_id;
        st_img->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    }
    st->time_base         = (AVRational){ 1, avctx->sample_rate };

    if (avf->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    set_metadata(ctx, t, avf);

    if (avcodec_open2(avctx, codec, NULL) < 0) {
        cyanrip_log(ctx, 0, "Could not open codec!\n");
        goto fail;
    }

    if (avcodec_parameters_from_context(st->codecpar, avctx) < 0) {
        cyanrip_log(ctx, 0, "Couldn't copy codec params!\n");
        goto fail;
    }

    /* Debug */
    av_dump_format(avf, 0, filename, 1);

    if ((ret = avio_open(&avf->pb, filename, AVIO_FLAG_WRITE)) < 0) {
        cyanrip_log(ctx, 0, "Couldn't open %s - %s!\n", filename, av_err2str(ret));
        goto fail;
    }

    if ((ret = avformat_write_header(avf, NULL)) < 0) {
        cyanrip_log(ctx, 0, "Couldn't write header - %s!\n", av_err2str(ret));
        goto fail;
    }

    if (ctx->cover_image_pkt) {
        AVPacket *pkt = ctx->cover_image_pkt;
        pkt->stream_index = 1;
        ret = av_interleaved_write_frame(avf, pkt);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error while writing packet - %s!\n", av_err2str(ret));
            goto fail;
        }
    }

    int eof_met = 0;
    int16_t *src_samples = t->samples;
    int samples_left = t->nb_samples;
    while (!eof_met) {
        AVFrame *frame = NULL;
        if (samples_left > 0) {
            frame                 = av_frame_alloc();
            frame->format         = avctx->sample_fmt;
            frame->channel_layout = avctx->channel_layout;
            frame->sample_rate    = avctx->sample_rate;
            frame->nb_samples     = FFMIN(samples_left >> 1, avctx->frame_size);
            frame->pts            = t->nb_samples*2 - samples_left*2;
            av_frame_get_buffer(frame, 0);
            frame->extended_data[0] = frame->data[0];
            memcpy(frame->data[0], src_samples, frame->nb_samples*4);
            src_samples  += frame->nb_samples*2;
            samples_left -= frame->nb_samples*2;
        }

        avcodec_send_frame(avctx, frame);
        while (1) {
            AVPacket pkt = { 0 };
            av_init_packet(&pkt);
            ret = avcodec_receive_packet(avctx, &pkt);
            if (ret == AVERROR_EOF) {
                eof_met = 1;
                break;
            } else if (ret == AVERROR(EAGAIN)) {
                break;
            } else if (ret < 0) {
                cyanrip_log(ctx, 0, "Error while encoding!\n");
                goto fail;
            }
            pkt.stream_index = 0;
            ret = av_interleaved_write_frame(avf, &pkt);
            if (ret < 0) {
                cyanrip_log(ctx, 0, "Error while writing packet - %s!\n", av_err2str(ret));
                goto fail;
            }
            av_packet_unref(&pkt);
        }
        av_frame_free(&frame);
    }

    av_write_trailer(avf);
    avcodec_close(avctx);
    avio_closep(&avf->pb);
    avformat_free_context(avf);
    av_free(avctx);

    return 0;

fail:

    return 1;
}
