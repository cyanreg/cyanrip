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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include "cyanrip_encode.h"
#include "cyanrip_log.h"
#include "os_compat.h"

typedef struct cyanrip_out_fmt {
    const char *name;
    const char *ext;
    int coverart_supported;
    int compression_level;
    int codec;
} cyanrip_out_fmt;

cyanrip_out_fmt fmt_map[] = {
    [CYANRIP_FORMAT_FLAC]    = { "FLAC",    "flac",  0, 11, AV_CODEC_ID_FLAC     },
    [CYANRIP_FORMAT_MP3]     = { "MP3",     "mp3",   1,  0, AV_CODEC_ID_MP3      },
    [CYANRIP_FORMAT_TTA]     = { "TTA",     "tta",   0,  0, AV_CODEC_ID_TTA      },
    [CYANRIP_FORMAT_OPUS]    = { "OPUS",    "opus",  0, 10, AV_CODEC_ID_OPUS     },
    [CYANRIP_FORMAT_AAC]     = { "AAC",     "m4a",   0,  0, AV_CODEC_ID_AAC,     },
    [CYANRIP_FORMAT_WAVPACK] = { "WAVPACK", "wv",    0,  8, AV_CODEC_ID_WAVPACK, },
    [CYANRIP_FORMAT_VORBIS]  = { "VORBIS",  "ogg",   0,  0, AV_CODEC_ID_VORBIS,  },
    [CYANRIP_FORMAT_ALAC]    = { "ALAC",    "m4a",   0,  2, AV_CODEC_ID_ALAC,    },
};

void cyanrip_print_codecs(void)
{
    cyanrip_init_encoding(NULL);
    for (int i = 0; i < CYANRIP_FORMATS_NB; i++) {
        cyanrip_out_fmt *cfmt = &fmt_map[i];
        const AVCodecDescriptor *cd = avcodec_descriptor_get(cfmt->codec);
        if (avcodec_find_encoder(cfmt->codec))
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
    if (!ctx->settings.cover_image_path) {
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
    av_dict_set_int(&avf->metadata, "tracktotal", ctx->drive->tracks,0);
}

static const uint64_t get_codec_channel_layout(AVCodec *codec)
{
    int i = 0;
    if (!codec->channel_layouts)
        return AV_CH_LAYOUT_STEREO;
    while (1) {
        if (!codec->channel_layouts[i])
            break;
        if (codec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
            return codec->channel_layouts[i];
        i++;
    }
    return codec->channel_layouts[0];
}

static enum AVSampleFormat get_codec_sample_fmt(AVCodec *codec)
{
    int i = 0;
    if (!codec->sample_fmts)
        return AV_SAMPLE_FMT_S16;
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

static int get_codec_sample_rate(AVCodec *codec)
{
    int i = 0;
    if (!codec->supported_samplerates)
        return 44100;
    while (1) {
        if (!codec->supported_samplerates[i])
            break;
        if (codec->supported_samplerates[i] >= 44100)
            return codec->supported_samplerates[i];
        i++;
    }
    return codec->supported_samplerates[0];
}

static AVCodecContext *setup_out_avctx(cyanrip_ctx *ctx, AVFormatContext *avf,
                                       AVCodec *codec, cyanrip_out_fmt *cfmt)
{
    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        return NULL;

    avctx->opaque            = ctx;
    avctx->bit_rate          = lrintf(ctx->settings.bitrate*1000.0f);
    avctx->sample_fmt        = get_codec_sample_fmt(codec);
    avctx->channel_layout    = get_codec_channel_layout(codec);
    avctx->compression_level = cfmt->compression_level;
    avctx->sample_rate       = get_codec_sample_rate(codec);
    avctx->time_base         = (AVRational){ 1, avctx->sample_rate };
    avctx->channels          = av_get_channel_layout_nb_channels(avctx->channel_layout);
    if (ctx->settings.fast_mode)
        avctx->compression_level = 0;

    if (avf->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return avctx;
}

static AVCodecContext *spawn_in_avctx(cyanrip_ctx *ctx)
{
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_PCM_S16LE);
    if (!codec)
        return NULL;

    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        return NULL;

    avctx->channel_layout    = AV_CH_LAYOUT_STEREO;
    avctx->sample_rate       = 44100;
    avctx->channels          = 2;
    avctx->time_base         = (AVRational){ 1, 44100 };

    if (avcodec_open2(avctx, codec, NULL) < 0) {
        cyanrip_log(ctx, 0, "Could not open output codec context!\n");
        av_free(&avctx);
        return NULL;
    }

    return avctx;
}

static SwrContext *setup_init_swr(cyanrip_ctx *ctx, AVCodecContext *in_avctx,
                                  AVCodecContext *out_avctx)
{
    SwrContext *swr = swr_alloc();
    if (!swr) {
        cyanrip_log(ctx, 0, "Could not alloc swr context!\n");
        return NULL;
    }

    av_opt_set_int           (swr, "in_sample_rate",     in_avctx->sample_rate,     0);
    av_opt_set_channel_layout(swr, "in_channel_layout",  in_avctx->channel_layout,  0);
    av_opt_set_sample_fmt    (swr, "in_sample_fmt",      in_avctx->sample_fmt,      0);

    av_opt_set_int           (swr, "out_sample_rate",    out_avctx->sample_rate,    0);
    av_opt_set_channel_layout(swr, "out_channel_layout", out_avctx->channel_layout, 0);
    av_opt_set_sample_fmt    (swr, "out_sample_fmt",     out_avctx->sample_fmt,     0);

    if (swr_init(swr) < 0) {
        cyanrip_log(ctx, 0, "Could not init swr context!\n");
        swr_free(&swr);
        return NULL;
    }

    return swr;
}

int cyanrip_encode_track(cyanrip_ctx *ctx, cyanrip_track *t,
                         enum cyanrip_output_formats format)
{
    int ret, status = 1;
    cyanrip_out_fmt *cfmt = &fmt_map[format];

    AVFormatContext *avf = NULL;
    SwrContext *swr = NULL;
    AVCodecContext *in_avctx = NULL;
    AVStream *st = NULL;
    AVStream *st_img = NULL;
    AVCodec *out_codec = NULL;
    AVCodecContext *out_avctx = NULL;

    char dirname[259], filename[1024], disc_name[256], track_name[256];
    strcpy(disc_name, ctx->disc_name);
    strcpy(track_name, t->name);

    if (ctx->settings.base_dst_folder)
        sprintf(dirname, "%s [%s]", ctx->settings.base_dst_folder, cfmt->name);
    else if (strlen(ctx->disc_name))
        sprintf(dirname, "%s [%s]", cyanrip_sanitize_fn(disc_name), cfmt->name);
    else if (strlen(ctx->discid))
        sprintf(dirname, "%s [%s]", ctx->discid, cfmt->name);
    else
        sprintf(dirname, "%s [%s]", "CR_Album", cfmt->name);

    if (strlen(t->name))
        sprintf(filename, "%s/%02i - %s.%s", dirname, t->index + 1,
                cyanrip_sanitize_fn(track_name), cfmt->ext);
    else
        sprintf(filename, "%s/%02i.%s", dirname, t->index + 1, cfmt->ext);

    struct stat st_req = { 0 };
    if (stat(dirname, &st_req) == -1)
        mkdir(dirname, 0700);

    /* lavf init */
    if (avformat_alloc_output_context2(&avf, NULL, NULL, filename) < 0) {
        cyanrip_log(ctx, 0, "Unable to init lavf context!\n");
        goto fail;
    }

    st = avformat_new_stream(avf, NULL);
    if (!st) {
        cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
        goto fail;
    }

    /* Cover image init */
    if (ctx->cover_image_pkt && cfmt->coverart_supported) {
        st_img = avformat_new_stream(avf, NULL);
        if (!st_img) {
            cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
            goto fail;
        }
        memcpy(st_img->codecpar, ctx->cover_image_params, sizeof(AVCodecParameters));
        st_img->disposition |= AV_DISPOSITION_ATTACHED_PIC;
        st_img->time_base = (AVRational){ 1, 25 };
        avf->oformat->video_codec = st_img->codecpar->codec_id;
    }

    /* Find encoder */
    out_codec = avcodec_find_encoder(cfmt->codec);
    if (!out_codec) {
        cyanrip_log(ctx, 0, "Codec not found (not compiled in lavc?)!\n");
        goto fail;
    }
    avf->oformat->audio_codec = out_codec->id;

    /* Output avctx */
    out_avctx = setup_out_avctx(ctx, avf, out_codec, cfmt);
    if (!out_avctx) {
        cyanrip_log(ctx, 0, "Unable to init output avctx!\n");
        goto fail;
    }
    st->id        = 0;
    st->time_base = (AVRational){ 1, out_avctx->sample_rate };

    /* Encode metadata */
    set_metadata(ctx, t, avf);

    /* Open encoder */
    if (avcodec_open2(out_avctx, out_codec, NULL) < 0) {
        cyanrip_log(ctx, 0, "Could not open output codec context!\n");
        goto fail;
    }

    if (avcodec_parameters_from_context(st->codecpar, out_avctx) < 0) {
        cyanrip_log(ctx, 0, "Couldn't copy codec params!\n");
        goto fail;
    }

    /* Debug print */
    av_dump_format(avf, 0, filename, 1);

    /* Open for writing */
    if ((ret = avio_open(&avf->pb, filename, AVIO_FLAG_WRITE)) < 0) {
        cyanrip_log(ctx, 0, "Couldn't open %s - %s! Invalid folder name? Try -D <folder>.\n", filename, av_err2str(ret));
        goto fail;
    }

    if ((ret = avformat_write_header(avf, NULL)) < 0) {
        cyanrip_log(ctx, 0, "Couldn't write header - %s!\n", av_err2str(ret));
        goto fail;
    }

    /* mux cover image */
    if (ctx->cover_image_pkt && cfmt->coverart_supported) {
        AVPacket *pkt = av_packet_clone(ctx->cover_image_pkt);
        pkt->stream_index = 1;
        if ((ret = av_interleaved_write_frame(avf, pkt)) < 0) {
            cyanrip_log(ctx, 0, "Error writing picture packet - %s!\n", av_err2str(ret));
            goto fail;
        }
    }

    /* Input decoder */
    in_avctx = spawn_in_avctx(ctx);
    if (!in_avctx)
        goto fail;

    /* SWR */
    swr = setup_init_swr(ctx, in_avctx, out_avctx);
    if (!swr)
        goto fail;

    int eof_met = 0;

    int samples_left = t->nb_samples;
    int16_t *src_samples = t->samples;

    while (!eof_met) {
        const int samples_avail = FFMIN(samples_left, samples_left);

        AVFrame *in_frame = NULL;

        if (samples_left) {
            AVPacket in_pkt = {
                .data = (uint8_t *)src_samples,
                .size = samples_avail << 1,
            };
            src_samples  += samples_avail;
            samples_left -= samples_avail;

            avcodec_send_packet(in_avctx, &in_pkt);
            in_frame = av_frame_alloc();
            ret = avcodec_receive_frame(in_avctx, in_frame);
            if (ret < 0) {
                cyanrip_log(ctx, 0, "Error decoding - %s!\n", av_err2str(ret));
                goto fail;
            }
        }

        AVFrame *out_frame        = av_frame_alloc();
        out_frame->format         = out_avctx->sample_fmt;
        out_frame->channel_layout = out_avctx->channel_layout;
        out_frame->sample_rate    = out_avctx->sample_rate;
        out_frame->nb_samples     = out_avctx->frame_size;
        out_frame->pts            = ROUNDED_DIV(swr_next_pts(swr, INT64_MIN), in_avctx->sample_rate);
        av_frame_get_buffer(out_frame, 0);
        out_frame->extended_data  = out_frame->data;

        cyanrip_log(NULL, 0, "\rEncoding track %i, progress - %0.2f%%", t->index + 1,
                    8820000.0f*(float)out_frame->pts/(t->nb_samples*out_frame->sample_rate));

        /* Convert */
        ret = swr_convert_frame(swr, out_frame, in_frame);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error resampling - %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Flush */
        if (!out_frame->nb_samples)
            av_frame_free(&out_frame);

        /* Give frame */
        ret = avcodec_send_frame(out_avctx, out_frame);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error encoding - %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Return */
        while (1) {
            AVPacket out_pkt;
            av_init_packet(&out_pkt);
            ret = avcodec_receive_packet(out_avctx, &out_pkt);
            if (ret == AVERROR_EOF) {
                eof_met = 1;
                break;
            } else if (ret == AVERROR(EAGAIN)) {
                break;
            } else if (ret < 0) {
                cyanrip_log(ctx, 0, "Error while encoding!\n");
                goto fail;
            }
            out_pkt.stream_index = 0;
            ret = av_interleaved_write_frame(avf, &out_pkt);
            if (ret < 0) {
                cyanrip_log(ctx, 0, "Error writing packet - %s!\n", av_err2str(ret));
                goto fail;
            }
            av_packet_unref(&out_pkt);
        }

        /* Free */
        av_frame_free(&in_frame);
        av_frame_free(&out_frame);
    }

    if ((ret = av_write_trailer(avf)) < 0) {
        cyanrip_log(ctx, 0, "Error writing trailer - %s!\n", av_err2str(ret));
        goto fail;
    }

    cyanrip_log(NULL, 0, "\r\nTrack %i encoded!\n", t->index + 1);

    status = 0;

fail:

    swr_free(&swr);

    avcodec_close(in_avctx);
    avcodec_close(out_avctx);

    avio_closep(&avf->pb);
    avformat_free_context(avf);

    av_free(in_avctx);
    av_free(out_avctx);

    return status;
}
