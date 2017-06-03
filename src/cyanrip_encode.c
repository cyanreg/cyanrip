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
#include <stdarg.h>
#include <sys/stat.h>
#ifdef __MINGW32__
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include "cyanrip_encode.h"
#include "cyanrip_log.h"

typedef struct cyanrip_out_fmt {
    const char *name;
    const char *ext;
    int resample;
    int coverart_supported;
    int codec;
    int sfmt;
    int rate;
} cyanrip_out_fmt;

cyanrip_out_fmt fmt_map[] = {
    [CYANRIP_FORMAT_FLAC]    = { "FLAC",    "flac",  0, 1, AV_CODEC_ID_FLAC,    AV_SAMPLE_FMT_S16,  44100 },
    [CYANRIP_FORMAT_MP3]     = { "MP3",     "mp3",   1, 1, AV_CODEC_ID_MP3,     AV_SAMPLE_FMT_S16P, 44100 },
    [CYANRIP_FORMAT_TTA]     = { "TTA",     "tta",   0, 0, AV_CODEC_ID_TTA,     AV_SAMPLE_FMT_S16,  44100 },
    [CYANRIP_FORMAT_OPUS]    = { "OPUS",    "opus",  1, 0, AV_CODEC_ID_OPUS,    AV_SAMPLE_FMT_FLT,  48000 },
    [CYANRIP_FORMAT_AAC]     = { "AAC",     "aac",   1, 0, AV_CODEC_ID_AAC,     AV_SAMPLE_FMT_FLTP, 44100 },
    [CYANRIP_FORMAT_WAVPACK] = { "WAVPACK", "wv",    1, 0, AV_CODEC_ID_WAVPACK, AV_SAMPLE_FMT_S16P, 44100 },
    [CYANRIP_FORMAT_VORBIS]  = { "OGG",     "ogg",   1, 0, AV_CODEC_ID_VORBIS,  AV_SAMPLE_FMT_FLTP, 44100 },
    [CYANRIP_FORMAT_ALAC]    = { "ALAC",    "m4a",   1, 0, AV_CODEC_ID_ALAC,    AV_SAMPLE_FMT_S16P, 44100 },
};

void cyanrip_print_codecs(void)
{
    for (int i = 0; i < CYANRIP_FORMATS_NB; i++) {
        cyanrip_out_fmt *cfmt = &fmt_map[i];
        const AVCodecDescriptor *cd = avcodec_descriptor_get(cfmt->codec);
        cyanrip_log(NULL, 0, "    %s\n", cd->name);
    }
}

int cyanrip_validate_fmt(const char *fmt)
{
    for (int i = 0; i < CYANRIP_FORMATS_NB; i++) {
        cyanrip_out_fmt *cfmt = &fmt_map[i];
        if (!strncasecmp(fmt, cfmt->name, strlen(cfmt->name)))
            return i;
    }
    return -1;
}

const char *cyanrip_fmt_desc(enum cyanrip_output_formats format)
{
    return format < CYANRIP_FORMATS_NB ? fmt_map[format].name : NULL;
}

void cyanrip_init_encoding(cyanrip_ctx *ctx)
{
    av_register_all();
}

void cyanrip_end_encoding(cyanrip_ctx *ctx)
{
    av_free(ctx->cover_image_pkt);
    av_free(ctx->cover_image_params);
}

int cyanrip_setup_cover_image(cyanrip_ctx *ctx)
{
    if (!ctx->settings.cover_image_path || !strlen(ctx->settings.cover_image_path)) {
        ctx->cover_image_pkt = NULL;
        return 0;
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

    ctx->cover_image_params = av_calloc(1, sizeof(AVCodecParameters));
    memcpy(ctx->cover_image_params, avf->streams[0]->codecpar,
           sizeof(AVCodecParameters));

    AVPacket *pkt = av_calloc(1, sizeof(AVPacket));
    av_init_packet(pkt);

    if (av_read_frame(avf, pkt) < 0) {
        cyanrip_log(ctx, 0, "Error demuxing cover image!\n");
        goto fail;
    }

    ctx->cover_image_pkt = pkt;

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
    strftime(t_s, sizeof(t_s), "%Y-%m-%dT%H:%M:%S", t_l);

    strftime(t_disc_date, sizeof(t_disc_date), "%Y-%m-%dT%H:%M:%S", ctx->disc_date);

#define ADD_TAG(dict, name, source, flags)          \
    do {                                            \
        if (strlen(source))                         \
            av_dict_set(dict, name, source, flags); \
    } while (0)

    ADD_TAG(&avf->metadata, "comment",            "cyanrip",         0);
    ADD_TAG(&avf->metadata, "title",              t->name,           0);
    ADD_TAG(&avf->metadata, "author",             t->artist,         0);
    ADD_TAG(&avf->metadata, "creation_time",      t_s,               0);
    ADD_TAG(&avf->metadata, "album",              ctx->disc_name,    0);
    ADD_TAG(&avf->metadata, "album_artist",       ctx->album_artist, 0);
    ADD_TAG(&avf->metadata, "date",               t_disc_date,       0);
    ADD_TAG(&avf->metadata, "musicbrainz_discid", ctx->discid,       0);
    av_dict_set_int(&avf->metadata, "track",      t->index + 1,      0);
}

int cyanrip_encode_track(cyanrip_ctx *ctx, cyanrip_track *t,
                         enum cyanrip_output_formats format)
{
    int ret;
    cyanrip_out_fmt *cfmt = &fmt_map[format];

    char dirname[259], filename[1024];

    if (strlen(ctx->disc_name))
        sprintf(dirname, "%s [%s]", ctx->disc_name, cfmt->name);
    else
        sprintf(dirname, "%s [%s]", ctx->discid, cfmt->name);
    if (strlen(t->name))
        sprintf(filename, "%s/%02i - %s.%s", dirname, t->index + 1, t->name, cfmt->ext);
    else
        sprintf(filename, "%s/%02i.%s", dirname, t->index + 1, cfmt->ext);

    struct stat st_req = { 0 };
    if (stat(dirname, &st_req) == -1)
        mkdir(dirname, 0700);

    AVFormatContext *avf = NULL;
    if (avformat_alloc_output_context2(&avf, NULL, NULL, filename) < 0) {
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
    if (ctx->cover_image_pkt && cfmt->coverart_supported) {
        st_img = avformat_new_stream(avf, NULL);
        if (!st_img) {
            cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
            goto fail;
        }
        memcpy(st_img->codecpar, ctx->cover_image_params, sizeof(AVCodecParameters));
        st_img->disposition |= AV_DISPOSITION_ATTACHED_PIC;
        st_img->time_base = (AVRational){ 1, 25 };
        fmt->video_codec = st_img->codecpar->codec_id;
    }

    AVCodec *codec = avcodec_find_encoder(cfmt->codec);
    if (!codec) {
        cyanrip_log(ctx, 0, "Codec not found!\n");
        goto fail;
    }
    fmt->audio_codec = cfmt->codec;

    SwrContext *swr = NULL;
    if (cfmt->resample) {
        swr = swr_alloc();
        if (!swr) {
            cyanrip_log(ctx, 0, "swr init failure!\n");
            goto fail;
        }

        av_opt_set_int(swr, "in_channel_layout",  AV_CH_LAYOUT_STEREO,  0);
        av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO,  0);
        av_opt_set_int(swr, "in_sample_rate",     44100,                0);
        av_opt_set_int(swr, "out_sample_rate",    cfmt->rate,           0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt",  AV_SAMPLE_FMT_S16, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", cfmt->sfmt,        0);

        if (swr_init(swr) < 0) {
            cyanrip_log(ctx, 0, "swr init failure!\n");
            goto fail;
        }
    }

    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx) {
        cyanrip_log(ctx, 0, "Unable to init avctx!\n");
        goto fail;
    }

    avctx->opaque         = ctx;
    avctx->bit_rate       = lrintf(ctx->settings.bitrate*1000.0f);
    avctx->sample_fmt     = cfmt->sfmt;
    avctx->channel_layout = AV_CH_LAYOUT_STEREO;
    avctx->sample_rate    = cfmt->rate;
    avctx->channels       = 2;
    st->id                = 0;
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

    if (ctx->cover_image_pkt && cfmt->coverart_supported) {
        AVPacket *pkt = av_packet_clone(ctx->cover_image_pkt);
        pkt->stream_index = 1;
        if ((ret = av_interleaved_write_frame(avf, pkt)) < 0) {
            cyanrip_log(ctx, 0, "Error writing picture packet - %s!\n", av_err2str(ret));
            goto fail;
        }
    }

    int swr_flush = 0;
    int eof_met = 0;
    int samples_done = 0;
    int samples_left = t->nb_samples;
    int16_t *src_samples = t->samples + (OVER_UNDER_READ_FRAMES*CDIO_CD_FRAMESIZE_RAW >> 1) + ctx->settings.offset*2;
    while (!eof_met) {
        AVFrame *frame = NULL;
        if (samples_left > 0 || (swr_flush == 1)) {
            frame                 = av_frame_alloc();
            frame->format         = avctx->sample_fmt;
            frame->channel_layout = avctx->channel_layout;
            frame->sample_rate    = avctx->sample_rate;
            frame->nb_samples     = FFMIN(samples_left >> 1, avctx->frame_size);
            if (swr_flush)
                frame->nb_samples = avctx->frame_size;
            av_frame_get_buffer(frame, 0);
            frame->extended_data  = frame->data;
            frame->pts            = samples_done;
            if (swr) {
                int ret_s;
                int in_s = swr_flush ? 0 : frame->nb_samples;
                const uint8_t *src[] = { (const uint8_t *)src_samples };
                AVRational cd_tb = (AVRational){ 1, 44100 };
                AVRational adj_tb = (AVRational){ 1, 44100 * avctx->sample_rate };
                ret_s = swr_convert(swr, frame->data, frame->nb_samples, src, in_s);
                frame->pts = swr_next_pts(swr, av_rescale_q(frame->pts, cd_tb, adj_tb));
                frame->pts /= 44100;
                samples_done += swr_flush ? 0 : frame->nb_samples;
                if (swr_flush && !ret_s) {
                    av_frame_free(&frame);
                    swr_flush = 2;
                    continue;
                }
            } else {
                memcpy(frame->data[0], src_samples, frame->nb_samples*4);
                samples_done += frame->nb_samples*2;
            }
            src_samples  += frame->nb_samples*2;
            samples_left -= frame->nb_samples*2;
            if (samples_left <= 0 && swr && !swr_flush)
                swr_flush = 1;
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
                cyanrip_log(ctx, 0, "Error writing packet - %s!\n", av_err2str(ret));
                goto fail;
            }
            av_packet_unref(&pkt);
        }
        av_frame_free(&frame);
    }

    if ((ret = av_write_trailer(avf)) < 0) {
        cyanrip_log(ctx, 0, "Error writing trailer - %s!\n", av_err2str(ret));
        goto fail;
    }
    avcodec_close(avctx);
    avio_closep(&avf->pb);
    avformat_free_context(avf);
    av_free(avctx);

    return 0;

fail:

    return 1;
}
