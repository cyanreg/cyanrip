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
#include <stdatomic.h>
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

#include "frame_fifo.h"
#include "cyanrip_encode.h"
#include "cyanrip_log.h"
#include "os_compat.h"

struct cyanrip_enc_ctx {
    cyanrip_ctx *ctx;
    AVFrameFIFO fifo;
    pthread_t thread;
    AVFormatContext *avf;
    SwrContext *swr;
    AVCodecContext *out_avctx;
    atomic_int status;
};

struct cyanrip_dec_ctx {
    /* HDCD decoding */
    AVFilterGraph *graph;
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;

    /* Cover image demuxing */
    AVPacket *cover_image_pkt;
    AVCodecParameters *cover_image_params;
};

typedef struct cyanrip_out_fmt {
    const char *name;
    const char *folder_suffix;
    const char *ext;
    const char *lavf_name;
    int coverart_supported;
    int compression_level;
    int lossless;
    enum AVCodecID codec;
} cyanrip_out_fmt;

static const cyanrip_out_fmt fmt_map[] = {
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

void cyanrip_print_codecs(void)
{
    for (int i = 0; i < CYANRIP_FORMATS_NB; i++) {
        const cyanrip_out_fmt *cfmt = &fmt_map[i];
        if (avcodec_find_encoder(cfmt->codec) ||
            ((cfmt->codec == AV_CODEC_ID_NONE) &&
              avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE) &&
              avcodec_find_encoder(AV_CODEC_ID_PCM_S32LE))) {
            const char *str = cfmt->coverart_supported ? "\t(supports cover art)" : "";
            cyanrip_log(NULL, 0, "\t%s\tfolder: [%s]\textension: %s%s\n", cfmt->name, cfmt->folder_suffix, cfmt->ext, str);
        }
    }
}

int cyanrip_validate_fmt(const char *fmt)
{
    for (int i = 0; i < CYANRIP_FORMATS_NB; i++) {
        const cyanrip_out_fmt *cfmt = &fmt_map[i];
        if ((!strncasecmp(fmt, cfmt->name, strlen(fmt))) &&
            (strlen(fmt) == strlen(cfmt->name))) {
            if (cfmt->codec == AV_CODEC_ID_NONE &&
                avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE) &&
                avcodec_find_encoder(AV_CODEC_ID_PCM_S32LE))
                return i;
            else if (avcodec_find_encoder(cfmt->codec))
                return i;
            cyanrip_log(NULL, 0, "Encoder for %s not compiled in ffmpeg!\n", cfmt->name);
            return -1;
        }
    }
    return -1;
}

const char *cyanrip_fmt_desc(enum cyanrip_output_formats format)
{
    return format < CYANRIP_FORMATS_NB ? fmt_map[format].name : NULL;
}

const char *cyanrip_fmt_folder(enum cyanrip_output_formats format)
{
    return format < CYANRIP_FORMATS_NB ? fmt_map[format].folder_suffix : NULL;
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

static enum AVSampleFormat get_codec_sample_fmt(AVCodec *codec, int hdcd)
{
    int i = 0;
    if (!codec->sample_fmts)
        return hdcd ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= (2 + 2*hdcd))
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

static int get_codec_sample_rate(AVCodec *codec)
{
    int i = 0, ret;
    if (!codec->supported_samplerates)
        return 44100;

    /* Go to the array terminator (0) */
    while (codec->supported_samplerates[++i] > 0);
    /* Alloc, copy and sort array upwards */
    int *tmp = av_malloc(i*sizeof(int));
    memcpy(tmp, codec->supported_samplerates, i*sizeof(int));
    qsort(tmp, i, sizeof(int), cmp_numbers);

    /* Pick lowest one above 44100, otherwise just use the highest one */
    for (int j = 0; j < i; j++) {
        ret = tmp[j];
        if (ret >= 44100)
            break;
    }

    av_free(tmp);

    return ret;
}

static AVCodecContext *setup_out_avctx(cyanrip_ctx *ctx, AVFormatContext *avf,
                                       AVCodec *codec, const cyanrip_out_fmt *cfmt)
{
    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        return NULL;

    avctx->opaque            = ctx;
    avctx->bit_rate          = cfmt->lossless ? 0 : lrintf(ctx->settings.bitrate*1000.0f);
    avctx->sample_fmt        = get_codec_sample_fmt(codec, ctx->settings.decode_hdcd);
    avctx->channel_layout    = get_codec_channel_layout(codec);
    avctx->compression_level = cfmt->compression_level;
    avctx->sample_rate       = get_codec_sample_rate(codec);
    avctx->time_base         = (AVRational){ 1, avctx->sample_rate };
    avctx->channels          = av_get_channel_layout_nb_channels(avctx->channel_layout);

    if (cfmt->lossless && ctx->settings.decode_hdcd)
        avctx->bits_per_raw_sample = 24;
    else if (cfmt->lossless)
        avctx->bits_per_raw_sample = 16;

    if (avf->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return avctx;
}

void cyanrip_free_dec_ctx(cyanrip_ctx *ctx, cyanrip_dec_ctx **s)
{
    if (!s || !*s)
        return;

    cyanrip_dec_ctx *dec_ctx = *s;

    if (dec_ctx->graph) {
        cyanrip_set_av_log_capture(ctx, 1, 1, AV_LOG_INFO);
        avfilter_graph_free(&dec_ctx->graph);
        cyanrip_set_av_log_capture(ctx, 0, 0, 0);
    }
    av_packet_free(&dec_ctx->cover_image_pkt);
    av_freep(&dec_ctx->cover_image_params);
    av_freep(s);
}

static int cyanrip_read_cover_image(cyanrip_ctx *ctx, cyanrip_dec_ctx *dec_ctx,
                                    cyanrip_track *t)
{
    int ret = 0;
    AVFormatContext *avf = NULL;
    const char *cover_image_path = dict_get(t->meta, "cover_art");

    if (!cover_image_path)
        return 0;

    ret = avformat_open_input(&avf, cover_image_path, NULL, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to open \"%s\": %s!\n", cover_image_path,
                    av_err2str(ret));
        goto fail;
    }

    av_dict_set(&t->meta, "cover_art", NULL, 0);

    ret = avformat_find_stream_info(avf, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to get cover image info: %s!\n", av_err2str(ret));
        goto fail;
    }

    dec_ctx->cover_image_params = av_calloc(1, sizeof(AVCodecParameters));
    if (!dec_ctx->cover_image_params)
        goto fail;

    memcpy(dec_ctx->cover_image_params, avf->streams[0]->codecpar,
           sizeof(AVCodecParameters));

    dec_ctx->cover_image_pkt = av_packet_alloc();
    if (!dec_ctx->cover_image_pkt) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = av_read_frame(avf, dec_ctx->cover_image_pkt);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error demuxing cover image: %s!\n", av_err2str(ret));
        goto fail;
    }

    avformat_close_input(&avf);

    return 0;

fail:
    av_dict_set(&t->meta, "cover_art", NULL, 0);
    avformat_close_input(&avf);

    return ret;
}

static int init_hdcd_decoding(cyanrip_ctx *ctx, cyanrip_dec_ctx *s)
{
    int ret = 0;
    AVFilterInOut *outputs = NULL;
    AVFilterInOut *inputs = NULL;

    s->graph = avfilter_graph_alloc();
    if (!s->graph)
        return AVERROR(ENOMEM);

    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");

    char args[512];
    uint64_t layout = AV_CH_LAYOUT_STEREO;
    snprintf(args, sizeof(args),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
             1, 44100, 44100, av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), layout);

    ret = avfilter_graph_create_filter(&s->buffersrc_ctx, abuffersrc, "in",
                                       args, NULL, s->graph);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error creating filter source: %s!\n", av_err2str(ret));
        goto fail;
    }

    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    ret = avfilter_graph_create_filter(&s->buffersink_ctx, abuffersink, "out",
                                       NULL, NULL, s->graph);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error creating filter sink: %s!\n", av_err2str(ret));
        goto fail;
    }

    static const enum AVSampleFormat out_sample_fmts[] = { AV_SAMPLE_FMT_S32, -1 };
    ret = av_opt_set_int_list(s->buffersink_ctx, "sample_fmts", out_sample_fmts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error setting filter sample format: %s!\n", av_err2str(ret));
        goto fail;
    }

    static const int64_t out_channel_layouts[] = { AV_CH_LAYOUT_STEREO, -1 };
    ret = av_opt_set_int_list(s->buffersink_ctx, "channel_layouts", out_channel_layouts, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error setting filter channel layout: %s!\n", av_err2str(ret));
        goto fail;
    }

    static const int out_sample_rates[] = { 44100, -1 };
    ret = av_opt_set_int_list(s->buffersink_ctx, "sample_rates", out_sample_rates, -1,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error setting filter sample rate: %s!\n", av_err2str(ret));
        goto fail;
    }

    outputs = avfilter_inout_alloc();
    if (!outputs) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    outputs->name          = av_strdup("in");
    outputs->filter_ctx    = s->buffersrc_ctx;
    outputs->pad_idx       = 0;
    outputs->next          = NULL;

    inputs = avfilter_inout_alloc();
    if (!inputs) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    inputs->name          = av_strdup("out");
    inputs->filter_ctx    = s->buffersink_ctx;
    inputs->pad_idx       = 0;
    inputs->next          = NULL;

    const char *filter_desc = "hdcd";

    ret = avfilter_graph_parse_ptr(s->graph, filter_desc, &inputs, &outputs, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error parsing filter graph: %s!\n", av_err2str(ret));
        goto fail;
    }

    ret = avfilter_graph_config(s->graph, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error configuring filter graph: %s!\n", av_err2str(ret));
        goto fail;
    }

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return 0;

fail:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

int cyanrip_create_dec_ctx(cyanrip_ctx *ctx, cyanrip_dec_ctx **s,
                           cyanrip_track *t)
{
    int ret = 0;

    cyanrip_dec_ctx *dec_ctx = av_mallocz(sizeof(*dec_ctx));
    if (!dec_ctx)
        return AVERROR(ENOMEM);

    ret = cyanrip_read_cover_image(ctx, dec_ctx, t);
    if (ret < 0)
        goto fail;

    if (ctx->settings.decode_hdcd) {
        ret = init_hdcd_decoding(ctx, dec_ctx);
        if (ret < 0)
            goto fail;
    }

    *s = dec_ctx;

    return 0;

fail:
    cyanrip_free_dec_ctx(ctx, &dec_ctx);
    return ret;
}

static int push_frame_to_encs(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                              int num_enc, AVFrame *frame)
{
    int ret;

    for (int i = 0; i < num_enc; i++) {
        int status = atomic_load(&enc_ctx[i]->status);
        if (status < 0)
            return status;

        ret = push_to_fifo(&enc_ctx[i]->fifo, frame ? av_frame_clone(frame) : NULL);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error pushing frame to FIFO: %s!\n", av_err2str(ret));
            return ret;
        }
    }

    return 0;
}

static int filter_frame(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                        int num_enc, cyanrip_dec_ctx *dec_ctx, AVFrame *frame)
{
    int ret = 0;
    AVFrame *dec_frame = NULL;

    if (!dec_ctx->buffersrc_ctx)
        return push_frame_to_encs(ctx, enc_ctx, num_enc, frame);

    ret = av_buffersrc_add_frame_flags(dec_ctx->buffersrc_ctx, frame,
                                       AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT |
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error filtering frame: %s!\n", av_err2str(ret));
        goto fail;
    }

    while (1) {
        dec_frame = av_frame_alloc();
        if (!dec_frame) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = av_buffersink_get_frame(dec_ctx->buffersink_ctx, dec_frame);
        if (ret == AVERROR(EAGAIN)) {
            av_frame_free(&dec_frame);
            ret = 0;
            break;
        } else if (ret == AVERROR_EOF) {
            av_frame_free(&dec_frame);
            ret = 0;
            return push_frame_to_encs(ctx, enc_ctx, num_enc, NULL);
        } else if (ret < 0) {
            cyanrip_log(ctx, 0, "Error filtering frame: %s!\n", av_err2str(ret));
            goto fail;
        }

        push_frame_to_encs(ctx, enc_ctx, num_enc, dec_frame);
        av_frame_free(&dec_frame);
    }

fail:
    return ret;
}

int cyanrip_send_pcm_to_encoders(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                                 int num_enc, cyanrip_dec_ctx *dec_ctx,
                                 const uint8_t *data, int bytes)
{
    int ret = 0;
    AVFrame *frame = NULL;

    if (!data && !bytes)
        goto send;
    else if (!bytes)
        return 0;

    frame = av_frame_alloc();
    if (!frame) {
        cyanrip_log(ctx, 0, "Error allocating frame!\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    frame->sample_rate = 44100;
    frame->nb_samples = bytes >> 2;
    frame->channel_layout = AV_CH_LAYOUT_STEREO;
    frame->format = AV_SAMPLE_FMT_S16;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error allocating frame: %s!\n", av_err2str(ret));
        goto fail;
    }

    memcpy(frame->data[0], data, bytes);

send:
    ret = filter_frame(ctx, enc_ctx, num_enc, dec_ctx, frame);
fail:
    av_frame_free(&frame);
    return ret;
}

static SwrContext *setup_init_swr(cyanrip_ctx *ctx, AVCodecContext *out_avctx, int hdcd)
{
    SwrContext *swr = swr_alloc();
    if (!swr) {
        cyanrip_log(ctx, 0, "Could not alloc swr context!\n");
        return NULL;
    }

    enum AVSampleFormat in_sample_fmt = hdcd ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;

    av_opt_set_int           (swr, "in_sample_rate",     44100,                     0);
    av_opt_set_channel_layout(swr, "in_channel_layout",  AV_CH_LAYOUT_STEREO,       0);
    av_opt_set_sample_fmt    (swr, "in_sample_fmt",      in_sample_fmt,             0);

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

int cyanrip_end_track_encoding(cyanrip_enc_ctx **s)
{
    cyanrip_enc_ctx *ctx;
    if (!s || !*s)
        return 0;

    ctx = *s;

    push_to_fifo(&ctx->fifo, NULL);
    pthread_join(ctx->thread, NULL);

    swr_free(&ctx->swr);

    avcodec_close(ctx->out_avctx);

    if (ctx->avf)
        avio_closep(&ctx->avf->pb);
    avformat_free_context(ctx->avf);

    free_fifo(&ctx->fifo);
    av_free(ctx->out_avctx);

    int status = ctx->status;
    av_freep(s);
    return status;
}

void *cyanrip_track_encoding(void *ctx)
{
    cyanrip_enc_ctx *s = ctx;
    int ret = 0, flushing = 0, codec_frame_size = s->out_avctx->frame_size;

    if (!codec_frame_size ||
        (s->out_avctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
        codec_frame_size = CDIO_CD_FRAMESIZE_RAW >> 1;

    while (1) {
        AVFrame *dec_frame = NULL;

        if (!flushing) {
            dec_frame = pop_from_fifo(&s->fifo);
            flushing = !dec_frame;
        }

        /* Resample the frame, can be NULL */
        ret = swr_convert_frame(s->swr, NULL, dec_frame);
        av_frame_free(&dec_frame);
        if (ret < 0) {
            cyanrip_log(s->ctx, 0, "Error resampling: %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Not enough output samples to get a frame */
        if (!flushing && (swr_get_out_samples(s->swr, 0) < codec_frame_size))
            continue;

        AVFrame *out_frame = NULL;
        if (!flushing || s->swr) {
            if (!(out_frame = av_frame_alloc())) {
                cyanrip_log(s->ctx, 0, "Error allocating memory!\n");
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            out_frame->format         = s->out_avctx->sample_fmt;
            out_frame->channel_layout = s->out_avctx->channel_layout;
            out_frame->sample_rate    = s->out_avctx->sample_rate;
            out_frame->nb_samples     = codec_frame_size;
            out_frame->pts            = ROUNDED_DIV(swr_next_pts(s->swr, INT64_MIN),
                                                    44100);

            /* Get frame buffer */
            ret = av_frame_get_buffer(out_frame, 0);
            if (ret) {
                cyanrip_log(s->ctx, 0, "Error allocating: %s!\n", av_err2str(ret));
                av_frame_free(&out_frame);
                goto fail;
            }

            /* Resample */
            ret = swr_convert_frame(s->swr, out_frame, NULL);
            if (ret < 0) {
                cyanrip_log(s->ctx, 0, "Error resampling: %s!\n", av_err2str(ret));
                av_frame_free(&out_frame);
                goto fail;
            }

            /* swr has been drained */
            if (!out_frame->nb_samples) {
                av_frame_free(&out_frame);
                swr_free(&s->swr);
            }
        }

        /* Give frame */
        ret = avcodec_send_frame(s->out_avctx, out_frame);
        av_frame_free(&out_frame);
        if (ret < 0) {
            cyanrip_log(s->ctx, 0, "Error encoding: %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Return */
        while (1) {
            AVPacket out_pkt;
            av_init_packet(&out_pkt);
            ret = avcodec_receive_packet(s->out_avctx, &out_pkt);
            if (ret == AVERROR_EOF) {
                ret = 0;
                goto write_trailer;
            } else if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                break;
            } else if (ret < 0) {
                cyanrip_log(s->ctx, 0, "Error while encoding: %s!\n", av_err2str(ret));
                goto fail;
            }

            out_pkt.stream_index = 0;

            ret = av_interleaved_write_frame(s->avf, &out_pkt);
            av_packet_unref(&out_pkt);
            if (ret < 0) {
                cyanrip_log(s->ctx, 0, "Error writing packet: %s!\n", av_err2str(ret));
                goto fail;
            }
        }
    }

write_trailer:
    if ((ret = av_write_trailer(s->avf)) < 0) {
        cyanrip_log(s->ctx, 0, "Error writing trailer: %s!\n", av_err2str(ret));
        goto fail;
    }

fail:
    atomic_store(&s->status, ret);

    return NULL;
}

int cyanrip_init_track_encoding(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                                cyanrip_dec_ctx *dec_ctx, cyanrip_track *t,
                                enum cyanrip_output_formats format)
{
    int ret = 0;
    char *filename = NULL;
    const cyanrip_out_fmt *cfmt = &fmt_map[format];
    cyanrip_enc_ctx *s = av_mallocz(sizeof(*s));

    AVStream *st_aud = NULL;
    AVStream *st_img = NULL;
    AVCodec *out_codec = NULL;

    s->ctx = ctx;
    atomic_init(&s->status, 0);

    if (dict_get(t->meta, "title"))
        filename = av_asprintf("%s [%s]/%02i - %s.%s", ctx->base_dst_folder,
                               cfmt->folder_suffix, t->number,
                               cyanrip_sanitize_fn(dict_get(t->meta, "title")),
                               cfmt->ext);
    else
        filename = av_asprintf("%s [%s]/%02i.%s", ctx->base_dst_folder,
                               cfmt->folder_suffix, t->number, cfmt->ext);

    /* lavf init */
    ret = avformat_alloc_output_context2(&s->avf, NULL, cfmt->lavf_name, filename);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to init lavf context: %s!\n", av_err2str(ret));
        goto fail;
    }

    st_aud = avformat_new_stream(s->avf, NULL);
    if (!st_aud) {
        cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* Cover image init */
    if (dec_ctx->cover_image_pkt && cfmt->coverart_supported) {
        st_img = avformat_new_stream(s->avf, NULL);
        if (!st_img) {
            cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        memcpy(st_img->codecpar, dec_ctx->cover_image_params,
               sizeof(AVCodecParameters));
        st_img->disposition |= AV_DISPOSITION_ATTACHED_PIC;
        st_img->time_base = (AVRational){ 1, 25 };
        s->avf->oformat->video_codec = st_img->codecpar->codec_id;
    }

    /* Find encoder */
    if (cfmt->codec == AV_CODEC_ID_NONE)
        out_codec = avcodec_find_encoder(ctx->settings.decode_hdcd ?
                                         AV_CODEC_ID_PCM_S32LE :
                                         AV_CODEC_ID_PCM_S16LE);
    else
        out_codec = avcodec_find_encoder(cfmt->codec);

    if (!out_codec) {
        cyanrip_log(ctx, 0, "Codec not found (not compiled in lavc?)!\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    s->avf->oformat->audio_codec = out_codec->id;

    if (format == CYANRIP_FORMAT_OPUS_MP4) {
        cyanrip_log(ctx, 0, "Opus mapping in MP4 is experimental, "
                    "use at your own risk.\n");
        s->avf->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }

    /* Output avctx */
    s->out_avctx = setup_out_avctx(ctx, s->avf, out_codec, cfmt);
    if (!s->out_avctx) {
        cyanrip_log(ctx, 0, "Unable to init output avctx!\n");
        goto fail;
    }

    /* Set primary audio stream's parameters */
    st_aud->id        = 0;
    st_aud->time_base = (AVRational){ 1, s->out_avctx->sample_rate };

    /* Add metadata */
    av_dict_copy(&s->avf->metadata, t->meta, 0);

    /* Open encoder */
    ret = avcodec_open2(s->out_avctx, out_codec, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Could not open output codec context!\n");
        goto fail;
    }

    /* Set codecpar */
    ret = avcodec_parameters_from_context(st_aud->codecpar, s->out_avctx);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Couldn't copy codec params!\n");
        goto fail;
    }

    /* Open for writing */
    ret = avio_open(&s->avf->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Couldn't open %s: %s! Invalid folder name? Try -D <folder>.\n", filename, av_err2str(ret));
        goto fail;
    }
    av_freep(&filename);

    /* Write header */
    ret = avformat_write_header(s->avf, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Couldn't write header: %s!\n", av_err2str(ret));
        goto fail;
    }

    /* Mux cover image */
    if (dec_ctx->cover_image_pkt && cfmt->coverart_supported) {
        AVPacket *pkt = av_packet_clone(dec_ctx->cover_image_pkt);
        pkt->stream_index = 1;
        if ((ret = av_interleaved_write_frame(s->avf, pkt)) < 0) {
            cyanrip_log(ctx, 0, "Error writing picture packet: %s!\n", av_err2str(ret));
            goto fail;
        }
    }

    /* SWR */
    s->swr = setup_init_swr(ctx, s->out_avctx, ctx->settings.decode_hdcd);
    if (!s->swr)
        goto fail;

    /* FIFO */
    init_fifo(&s->fifo, ctx->settings.enc_fifo_size);

    pthread_create(&s->thread, NULL, cyanrip_track_encoding, s);

    *enc_ctx = s;

    return 0;

fail:
    av_free(filename);
    cyanrip_end_track_encoding(&s);

    return ret;
}
