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

#include "fifo_frame.h"
#include "fifo_packet.h"
#include "cyanrip_encode.h"
#include "cyanrip_log.h"
#include "os_compat.h"

#if CONFIG_BIG_ENDIAN
#define AV_CODEC_ID_PCM_S16 AV_CODEC_ID_PCM_S16BE
#define AV_CODEC_ID_PCM_S32 AV_CODEC_ID_PCM_S32BE
#define AV_CODEC_ID_PCM_F64 AV_CODEC_ID_PCM_F64BE
#else
#define AV_CODEC_ID_PCM_S16 AV_CODEC_ID_PCM_S16LE
#define AV_CODEC_ID_PCM_S32 AV_CODEC_ID_PCM_S32LE
#define AV_CODEC_ID_PCM_F64 AV_CODEC_ID_PCM_F64LE
#endif

struct cyanrip_enc_ctx {
    cyanrip_ctx *ctx;
    AVBufferRef *fifo;
    pthread_t thread;
    AVFormatContext *avf;
    SwrContext *swr;
    AVCodecContext *out_avctx;
    atomic_int status;
    atomic_int quit;
    int audio_stream_index;
    cyanrip_track *t;

    pthread_mutex_t lock;

    AVStream *st_aud;
    AVStream *st_img;
    const cyanrip_out_fmt *cfmt;
    int separate_writeout;
    AVBufferRef *packet_fifo;
    AVPacket *cover_art_pkt;
};

typedef struct cyanrip_filt_ctx {
    /* Deemphasis, and HDCD decoding (not at once) */
    AVFilterGraph *graph;
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
} cyanrip_filt_ctx;

struct cyanrip_dec_ctx {
    cyanrip_filt_ctx filt;
    cyanrip_filt_ctx peak;
};

void cyanrip_print_codecs(void)
{
    for (int i = 0; i < CYANRIP_FORMATS_NB; i++) {
        const cyanrip_out_fmt *cfmt = &crip_fmt_info[i];
        int is_supported = 0;
        if (cfmt->codec == AV_CODEC_ID_NONE) {
            is_supported = avcodec_find_encoder(AV_CODEC_ID_PCM_S16) &&
                           avcodec_find_encoder(AV_CODEC_ID_PCM_S32) &&
                           avcodec_find_encoder(AV_CODEC_ID_PCM_F64);
        } else {
            is_supported = !!avcodec_find_encoder(cfmt->codec);
        }

        if (is_supported) {
            const char *str = cfmt->coverart_supported ? "\t(supports cover art)" : "";
            cyanrip_log(NULL, 0, "\t%s\tfolder: [%s]\textension: %s%s\n", cfmt->name, cfmt->folder_suffix, cfmt->ext, str);
        }
    }
}

int cyanrip_validate_fmt(const char *fmt)
{
    for (int i = 0; i < CYANRIP_FORMATS_NB; i++) {
        const cyanrip_out_fmt *cfmt = &crip_fmt_info[i];
        if ((!strncasecmp(fmt, cfmt->name, strlen(fmt))) &&
            (strlen(fmt) == strlen(cfmt->name))) {

            if (cfmt->codec == AV_CODEC_ID_NONE) {
                if (avcodec_find_encoder(AV_CODEC_ID_PCM_S16) &&
                    avcodec_find_encoder(AV_CODEC_ID_PCM_S32) &&
                    avcodec_find_encoder(AV_CODEC_ID_PCM_F64))
                    return i;
            } else if (avcodec_find_encoder(cfmt->codec)) {
                return i;
            } else {
                cyanrip_log(NULL, 0, "Encoder for %s not compiled in ffmpeg!\n", cfmt->name);
                return -1;
            }
        }
    }
    return -1;
}

const char *cyanrip_fmt_desc(enum cyanrip_output_formats format)
{
    return format < CYANRIP_FORMATS_NB ? crip_fmt_info[format].name : NULL;
}

const char *cyanrip_fmt_folder(enum cyanrip_output_formats format)
{
    return format < CYANRIP_FORMATS_NB ? crip_fmt_info[format].folder_suffix : NULL;
}

static const AVChannelLayout pick_codec_channel_layout(const AVCodec *codec)
{
    int i = 0;
    int max_channels = 0;
    AVChannelLayout ilayout = AV_CHANNEL_LAYOUT_STEREO;
    int in_channels = ilayout.nb_channels;
    AVChannelLayout best_layout = { 0 };

    /* Supports anything */
    if (!codec->ch_layouts)
        return ilayout;

    /* Try to match */
    while (1) {
        if (!codec->ch_layouts[i].nb_channels)
            break;
        if (!av_channel_layout_compare(&codec->ch_layouts[i], &ilayout))
            return codec->ch_layouts[i];
        i++;
    }

    i = 0;

    /* Try to match channel counts */
    while (1) {
        if (!codec->ch_layouts[i].nb_channels)
            break;
        int num = codec->ch_layouts[i].nb_channels;
        if (num > max_channels) {
            max_channels = num;
            best_layout = codec->ch_layouts[i];
        }
        if (num >= in_channels)
            return codec->ch_layouts[i];
        i++;
    }

    /* Whatever */
    return best_layout;
}

static enum AVSampleFormat pick_codec_sample_fmt(const AVCodec *codec, int hdcd)
{
    int i = 0;
    int max_bps = 0;
    int ibps = hdcd ? 20 : 16;
    enum AVSampleFormat ifmt = hdcd ? AV_SAMPLE_FMT_S32 :
                                      AV_SAMPLE_FMT_S16;
    enum AVSampleFormat ifmt_p = hdcd ? AV_SAMPLE_FMT_S32P :
                                        AV_SAMPLE_FMT_S16P;
    enum AVSampleFormat max_bps_fmt = AV_SAMPLE_FMT_NONE;

    ibps = ibps >> 3;

    /* Accepts anything */
    if (!codec->sample_fmts)
        return ifmt;

    /* Try to match the input sample format first */
    while (1) {
        if (codec->sample_fmts[i] == AV_SAMPLE_FMT_NONE)
            break;
        if (codec->sample_fmts[i] == ifmt ||
            codec->sample_fmts[i] == ifmt_p)
            return codec->sample_fmts[i];
        i++;
    }

    i = 0;

    /* Try to match bits per sample */
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        int bps = av_get_bytes_per_sample(codec->sample_fmts[i]);
        if (bps > max_bps) {
            max_bps = bps;
            max_bps_fmt = codec->sample_fmts[i];
        }
        if (bps == ibps)
            return codec->sample_fmts[i];
        i++;
    }

    /* Return the best one */
    return max_bps_fmt;
}

static int pick_codec_sample_rate(const AVCodec *codec)
{
    int i = 0, ret;
    int irate = 44100;

    if (!codec->supported_samplerates)
        return irate;

    /* Go to the array terminator (0) */
    while (codec->supported_samplerates[++i] > 0);
    /* Alloc, copy and sort array upwards */
    int *tmp = av_malloc(i*sizeof(int));
    memcpy(tmp, codec->supported_samplerates, i*sizeof(int));
    qsort(tmp, i, sizeof(int), cmp_numbers);

    /* Pick lowest one above the input rate, otherwise just use the highest one */
    for (int j = 0; j < i; j++) {
        ret = tmp[j];
        if (ret >= irate)
            break;
    }

    av_free(tmp);

    return ret;
}

static AVCodecContext *setup_out_avctx(cyanrip_ctx *ctx, AVFormatContext *avf,
                                       const AVCodec *codec, const cyanrip_out_fmt *cfmt,
                                       int decode_hdcd, int deemphasis)
{
    AVCodecContext *avctx = avcodec_alloc_context3(codec);
    if (!avctx)
        return NULL;

    avctx->opaque                = ctx;
    avctx->bit_rate              = cfmt->lossless ? 0 : lrintf(ctx->settings.bitrate*1000.0f);
    avctx->sample_fmt            = pick_codec_sample_fmt(codec, decode_hdcd);
    avctx->ch_layout             = pick_codec_channel_layout(codec);
    avctx->compression_level     = cfmt->compression_level;
    avctx->sample_rate           = pick_codec_sample_rate(codec);
    avctx->time_base             = (AVRational){ 1, avctx->sample_rate };
    avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    if (cfmt->lossless && decode_hdcd)
        avctx->bits_per_raw_sample = FFMIN(24, av_get_bytes_per_sample(avctx->sample_fmt)*8);
    else if (cfmt->lossless)
        avctx->bits_per_raw_sample = 16;

    if (avf->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return avctx;
}

static void cyanrip_free_filt_ctx(cyanrip_ctx *ctx, cyanrip_filt_ctx *filt_ctx, int capture)
{
    if (filt_ctx->graph) {
        if (capture)
            cyanrip_set_av_log_capture(ctx, 1, AV_LOG_INFO);
        avfilter_graph_free(&filt_ctx->graph);
        if (capture)
            cyanrip_set_av_log_capture(ctx, 0, 0);
    }
}

void cyanrip_free_dec_ctx(cyanrip_ctx *ctx, cyanrip_dec_ctx **s)
{
    if (!s || !*s)
        return;

    cyanrip_dec_ctx *dec_ctx = *s;

    cyanrip_free_filt_ctx(ctx, &dec_ctx->filt, 0);
    cyanrip_free_filt_ctx(ctx, &dec_ctx->peak, 0);

    av_freep(s);
}

static int init_filtering(cyanrip_ctx *ctx, cyanrip_filt_ctx *s,
                          int hdcd, int deemphasis, int peak)
{
    int ret = 0;
    AVFilterInOut *inputs = NULL;
    AVFilterInOut *outputs = NULL;

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

    if (!peak) {
        const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
        ret = avfilter_graph_create_filter(&s->buffersink_ctx, abuffersink, "out",
                                           NULL, NULL, s->graph);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error creating filter sink: %s!\n", av_err2str(ret));
            goto fail;
        }

        static const enum AVSampleFormat out_sample_fmts_hdcd[] = { AV_SAMPLE_FMT_S32, -1 };
        static const enum AVSampleFormat out_sample_fmts_deemph[] = { AV_SAMPLE_FMT_DBLP, -1 };

        ret = av_opt_set_int_list(s->buffersink_ctx, "sample_fmts",
                                  hdcd ? out_sample_fmts_hdcd :
                                  deemphasis ? out_sample_fmts_deemph :
                                  NULL,
                                  -1, AV_OPT_SEARCH_CHILDREN);
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

    if (!peak) {
        inputs = avfilter_inout_alloc();
        if (!inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        inputs->name          = av_strdup("out");
        inputs->filter_ctx    = s->buffersink_ctx;
        inputs->pad_idx       = 0;
        inputs->next          = NULL;
    }

    const char *filter_desc = hdcd ? "hdcd" :
                              deemphasis ? "aemphasis=type=cd" :
                              peak ? "ebur128=peak=true,anullsink" :
                              NULL;

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

    if ((ctx->settings.decode_hdcd) ||
        (ctx->settings.deemphasis && t->preemphasis) ||
        (ctx->settings.force_deemphasis)) {
        ret = init_filtering(ctx, &dec_ctx->filt,
                             ctx->settings.decode_hdcd,
                             (ctx->settings.deemphasis && t->preemphasis) || ctx->settings.force_deemphasis,
                             0);
        if (ret < 0)
            goto fail;
    }

    ret = init_filtering(ctx, &dec_ctx->peak, 0, 0, 1);
    if (ret < 0)
        goto fail;

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

        ret = cr_frame_fifo_push(enc_ctx[i]->fifo, frame);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error pushing frame to FIFO: %s!\n", av_err2str(ret));
            return ret;
        }
    }

    return 0;
}

static int filter_frame(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                        int num_enc, cyanrip_dec_ctx *dec_ctx, AVFrame *frame,
                        int calc_global_peak)
{
    int ret = 0;
    AVFrame *dec_frame = NULL;

    ret = av_buffersrc_add_frame_flags(dec_ctx->peak.buffersrc_ctx, frame,
                                       AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT |
                                       AV_BUFFERSRC_FLAG_KEEP_REF | AV_BUFFERSRC_FLAG_PUSH);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error filtering frame: %s!\n", av_err2str(ret));
        goto fail;
    }

    if (frame && calc_global_peak) {
        ret = av_buffersrc_add_frame_flags(ctx->peak_ctx->peak.buffersrc_ctx, frame,
                                           AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT |
                                           AV_BUFFERSRC_FLAG_KEEP_REF | AV_BUFFERSRC_FLAG_PUSH);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error filtering frame: %s!\n", av_err2str(ret));
            goto fail;
        }
    }

    if (!dec_ctx->filt.buffersrc_ctx)
        return push_frame_to_encs(ctx, enc_ctx, num_enc, frame);

    ret = av_buffersrc_add_frame_flags(dec_ctx->filt.buffersrc_ctx, frame,
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

        ret = av_buffersink_get_frame_flags(dec_ctx->filt.buffersink_ctx, dec_frame,
                                            AV_BUFFERSINK_FLAG_NO_REQUEST);
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
                                 const uint8_t *data, int bytes,
                                 int calc_global_peak)
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
    frame->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    frame->format = AV_SAMPLE_FMT_S16;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error allocating frame: %s!\n", av_err2str(ret));
        goto fail;
    }

    memcpy(frame->data[0], data, bytes);

send:
    ret = filter_frame(ctx, enc_ctx, num_enc, dec_ctx, frame, calc_global_peak);
fail:
    av_frame_free(&frame);
    return ret;
}

int cyanrip_reset_encoding(cyanrip_ctx *ctx, cyanrip_track *t)
{
    for (int i = 0; i < ctx->settings.outputs_num; i++) {
        cyanrip_enc_ctx *s = t->enc_ctx[i];

        atomic_store(&s->quit, 1);
        if (s->separate_writeout)
            pthread_mutex_unlock(&s->lock);
    }

    for (int i = 0; i < ctx->settings.outputs_num; i++)
        cyanrip_end_track_encoding(&t->enc_ctx[i]);

    for (int i = 0; i < ctx->settings.outputs_num; i++)
        cyanrip_init_track_encoding(ctx, &t->enc_ctx[i], t,
                                    ctx->settings.outputs[i]);

    return 0;
}

int cyanrip_finalize_encoding(cyanrip_ctx *ctx, cyanrip_track *t)
{
    AVFilterContext *filt_ctx = t->dec_ctx->peak.graph->filters[1];

    av_opt_get_double(filt_ctx, "integrated", AV_OPT_SEARCH_CHILDREN, &t->ebu_integrated);
    av_opt_get_double(filt_ctx, "range", AV_OPT_SEARCH_CHILDREN, &t->ebu_range);
    av_opt_get_double(filt_ctx, "lra_low", AV_OPT_SEARCH_CHILDREN, &t->ebu_lra_low);
    av_opt_get_double(filt_ctx, "lra_high", AV_OPT_SEARCH_CHILDREN, &t->ebu_lra_high);
    av_opt_get_double(filt_ctx, "sample_peak", AV_OPT_SEARCH_CHILDREN, &t->ebu_sample_peak);
    av_opt_get_double(filt_ctx, "true_peak", AV_OPT_SEARCH_CHILDREN, &t->ebu_true_peak);

    cyanrip_free_filt_ctx(ctx, &t->dec_ctx->peak, 1);
    if (ctx->settings.decode_hdcd)
        cyanrip_log(ctx, 0, "\n  ");
    else
        cyanrip_log(ctx, 0, "\n");
    cyanrip_free_filt_ctx(ctx, &t->dec_ctx->filt, ctx->settings.decode_hdcd);

    return 0;
}

int cyanrip_initialize_ebur128(cyanrip_ctx *ctx)
{
    int ret = 0;

    cyanrip_dec_ctx *dec_ctx = av_mallocz(sizeof(*dec_ctx));
    if (!dec_ctx)
        return AVERROR(ENOMEM);
    ctx->peak_ctx = dec_ctx;

    ret = init_filtering(ctx, &dec_ctx->peak, 0, 0, 1);
    if (ret < 0)
        goto fail;

    return 0;

fail:
    cyanrip_finalize_ebur128(ctx, 0);
    return ret;
}

int cyanrip_finalize_ebur128(cyanrip_ctx *ctx, int log)
{
    cyanrip_dec_ctx *dec_ctx = ctx->peak_ctx;

    if (!log)
        goto end;

    AVFilterContext *filt_ctx = dec_ctx->peak.graph->filters[1];

    int ret = av_buffersrc_add_frame_flags(dec_ctx->peak.buffersrc_ctx, NULL,
                                           AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT |
                                           AV_BUFFERSRC_FLAG_KEEP_REF | AV_BUFFERSRC_FLAG_PUSH);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error filtering frame: %s!\n", av_err2str(ret));
        cyanrip_free_filt_ctx(ctx, &dec_ctx->peak, 0);
        av_freep(ctx->peak_ctx);
        return ret;
    }

    av_opt_get_double(filt_ctx, "integrated", AV_OPT_SEARCH_CHILDREN, &ctx->ebu_integrated);
    av_opt_get_double(filt_ctx, "range", AV_OPT_SEARCH_CHILDREN, &ctx->ebu_range);
    av_opt_get_double(filt_ctx, "lra_low", AV_OPT_SEARCH_CHILDREN, &ctx->ebu_lra_low);
    av_opt_get_double(filt_ctx, "lra_high", AV_OPT_SEARCH_CHILDREN, &ctx->ebu_lra_high);
    av_opt_get_double(filt_ctx, "sample_peak", AV_OPT_SEARCH_CHILDREN, &ctx->ebu_sample_peak);
    av_opt_get_double(filt_ctx, "true_peak", AV_OPT_SEARCH_CHILDREN, &ctx->ebu_true_peak);

    cyanrip_log(ctx, 0, "Album Loudness ");

end:
    if (ctx->peak_ctx) {
        cyanrip_free_filt_ctx(ctx, &ctx->peak_ctx->peak, log);
        if (log)
            cyanrip_log(ctx, 0, "\n");
    }

    av_freep(&ctx->peak_ctx);

    return 0;
}

static SwrContext *setup_init_swr(cyanrip_ctx *ctx, AVCodecContext *out_avctx,
                                  int hdcd, int deemphasis)
{
    SwrContext *swr = swr_alloc();
    if (!swr) {
        cyanrip_log(ctx, 0, "Could not alloc swr context!\n");
        return NULL;
    }

    AVChannelLayout ichl = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    enum AVSampleFormat in_sample_fmt = hdcd ? AV_SAMPLE_FMT_S32 :
                                        (deemphasis ? AV_SAMPLE_FMT_DBLP :
                                                      AV_SAMPLE_FMT_S16);

    av_opt_set_int       (swr, "in_sample_rate",  44100,                  0);
    av_opt_set_chlayout  (swr, "in_chlayout",     &ichl,                  0);
    av_opt_set_sample_fmt(swr, "in_sample_fmt",   in_sample_fmt,          0);

    av_opt_set_int       (swr, "out_sample_rate", out_avctx->sample_rate, 0);
    av_opt_set_chlayout  (swr, "out_chlayout",    &out_avctx->ch_layout,  0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt",  out_avctx->sample_fmt,  0);

    if (swr_init(swr) < 0) {
        cyanrip_log(ctx, 0, "Could not init swr context!\n");
        swr_free(&swr);
        return NULL;
    }

    return swr;
}

static int64_t get_next_audio_pts(cyanrip_enc_ctx *ctx, AVFrame *in)
{
    const int64_t m = (int64_t)44100 * ctx->out_avctx->sample_rate;
    const int64_t b = (int64_t)ctx->out_avctx->time_base.num * m;
    const int64_t c = ctx->out_avctx->time_base.den;
    const int64_t in_pts = in ? in->pts : AV_NOPTS_VALUE;

    int64_t npts = in_pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale(in_pts, b, c);

    npts = swr_next_pts(ctx->swr, npts);

    int64_t out_pts = av_rescale(npts, c, b);

    return out_pts;
}

static int audio_process_frame(cyanrip_enc_ctx *ctx, AVFrame **input, int flush)
{
    int ret;
    int frame_size = ctx->out_avctx->frame_size;

    int64_t resampled_frame_pts = get_next_audio_pts(ctx, *input);

    /* Resample the frame, can be NULL */
    ret = swr_convert_frame(ctx->swr, NULL, *input);

    av_frame_free(input);

    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error pushing audio for resampling: %s!\n", av_err2str(ret));
        return ret;
    }

    /* Not enough output samples to get a frame */
    if (!flush) {
        int out_samples = swr_get_out_samples(ctx->swr, 0);
        if (!frame_size && out_samples)
            frame_size = out_samples;
        else if (!out_samples || ((frame_size) && (out_samples < frame_size)))
            return AVERROR(EAGAIN);
    } else if (!frame_size && ctx->swr) {
        frame_size = swr_get_out_samples(ctx->swr, 0);
        if (!frame_size)
            return 0;
    }

    if (flush && !ctx->swr)
        return 0;

    AVFrame *out_frame = NULL;
    if (!(out_frame = av_frame_alloc())) {
        av_log(ctx, AV_LOG_ERROR, "Error allocating frame!\n");
        return AVERROR(ENOMEM);
    }

    out_frame->format                = ctx->out_avctx->sample_fmt;
    out_frame->ch_layout             = ctx->out_avctx->ch_layout;
    out_frame->sample_rate           = ctx->out_avctx->sample_rate;
    out_frame->pts                   = resampled_frame_pts;

    /* SWR sets this field to whatever it can output if it can't this much */
    out_frame->nb_samples            = frame_size;

    /* Get frame buffer */
    ret = av_frame_get_buffer(out_frame, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error allocating frame: %s!\n", av_err2str(ret));
        av_frame_free(&out_frame);
        return ret;
    }

    /* Resample */
    ret = swr_convert_frame(ctx->swr, out_frame, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error pulling resampled audio: %s!\n", av_err2str(ret));
        av_frame_free(&out_frame);
        return ret;
    }

    /* swr has been drained */
    if (!out_frame->nb_samples) {
        av_frame_free(&out_frame);
        swr_free(&ctx->swr);
    }

    *input = out_frame;

    return 0;
}

int cyanrip_end_track_encoding(cyanrip_enc_ctx **s)
{
    cyanrip_enc_ctx *ctx;
    if (!s || !*s)
        return 0;

    ctx = *s;

    cr_frame_fifo_push(ctx->fifo, NULL);
    pthread_join(ctx->thread, NULL);
    pthread_mutex_destroy(&ctx->lock);

    swr_free(&ctx->swr);

    avcodec_close(ctx->out_avctx);

    if (ctx->avf)
        avio_closep(&ctx->avf->pb);
    avformat_free_context(ctx->avf);

    av_buffer_unref(&ctx->fifo);
    av_buffer_unref(&ctx->packet_fifo);
    av_free(ctx->out_avctx);
    av_packet_free(&ctx->cover_art_pkt);

    int status = ctx->status;
    av_freep(s);
    return status;
}

static int open_output(cyanrip_ctx *ctx, cyanrip_enc_ctx *s)
{
    int ret;

    /* Add metadata */
    av_dict_copy(&s->avf->metadata, s->t->meta, 0);

    /* Write header */
    ret = avformat_write_header(s->avf, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Couldn't write header: %s!\n", av_err2str(ret));
        goto fail;
    }

    /* Mux cover image */
    if (s->cover_art_pkt) {
        AVPacket *pkt = av_packet_clone(s->cover_art_pkt);
        pkt->stream_index = s->st_img->index;
        if ((ret = av_interleaved_write_frame(s->avf, pkt)) < 0) {
            cyanrip_log(ctx, 0, "Error writing picture packet: %s!\n", av_err2str(ret));
            goto fail;
        }
        av_packet_free(&pkt);
    }

fail:
    return ret;
}

static void *cyanrip_track_encoding(void *ctx)
{
    cyanrip_enc_ctx *s = ctx;
    int ret = 0, flushing = 0;

    /* Allocate output packet */
    AVPacket *out_pkt = av_packet_alloc();
    if (!out_pkt) {
        ret = AVERROR(ENOMEM);
        cyanrip_log(s->ctx, 0, "Error while encoding: %s!\n", av_err2str(ret));
        goto fail;
    }

    while (1) {
        AVFrame *out_frame = NULL;

        if (!flushing) {
            out_frame = cr_frame_fifo_pop(s->fifo);
            flushing = !out_frame;
        }

        ret = audio_process_frame(ctx, &out_frame, flushing);
        if (ret == AVERROR(EAGAIN))
            continue;
        else if (ret)
            goto fail;

        /* Give frame */
        ret = avcodec_send_frame(s->out_avctx, out_frame);
        av_frame_free(&out_frame);
        if (ret < 0) {
            cyanrip_log(s->ctx, 0, "Error encoding: %s!\n", av_err2str(ret));
            goto fail;
        }

        /* Return loop */
        while (1) {
            ret = avcodec_receive_packet(s->out_avctx, out_pkt);
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

            int sid = s->audio_stream_index;
            out_pkt->stream_index = sid;

            AVRational src_tb = s->out_avctx->time_base;
            AVRational dst_tb = s->avf->streams[sid]->time_base;

            /* Rescale timestamps to container */
            av_packet_rescale_ts(out_pkt, src_tb, dst_tb);

            if (s->separate_writeout) {
                /* Put encoded frame in FIFO */
                ret = cr_packet_fifo_push(s->packet_fifo, out_pkt);
                if (ret < 0) {
                    cyanrip_log(ctx, 0, "Error pushing packet to FIFO: %s!\n", av_err2str(ret));
                    goto fail;
                }
            } else {
                /* Send frame to lavf */
                ret = av_interleaved_write_frame(s->avf, out_pkt);
                if (ret < 0) {
                    cyanrip_log(s->ctx, 0, "Error writing packet: %s!\n", av_err2str(ret));
                    goto fail;
                }
             }

            /* Reset the packet */
            av_packet_unref(out_pkt);
        }
    }

write_trailer:
    av_packet_free(&out_pkt);

    if (s->separate_writeout) {
        ret = cr_packet_fifo_push(s->packet_fifo, NULL);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error pushing packet to FIFO: %s!\n", av_err2str(ret));
            goto fail;
        }

        pthread_mutex_lock(&s->lock);
        pthread_mutex_unlock(&s->lock);

        if (atomic_load(&s->quit))
            goto fail;

        if ((ret = open_output(s->ctx, s)) < 0) {
            cyanrip_log(s->ctx, 0, "Error writing to file: %s!\n", av_err2str(ret));
            goto fail;
        }

        while ((out_pkt = cr_packet_fifo_pop(s->packet_fifo))) {
            /* Send frames to lavf */
            ret = av_interleaved_write_frame(s->avf, out_pkt);
            av_packet_free(&out_pkt);
            if (ret < 0) {
                cyanrip_log(s->ctx, 0, "Error writing packet: %s!\n", av_err2str(ret));
                goto fail;
            }
        }
    }

    if ((ret = av_write_trailer(s->avf)) < 0) {
        cyanrip_log(s->ctx, 0, "Error writing trailer: %s!\n", av_err2str(ret));
        goto fail;
    }

fail:
    av_packet_free(&out_pkt);

    atomic_store(&s->status, ret);

    return NULL;
}

int cyanrip_writeout_track(cyanrip_ctx *ctx, cyanrip_enc_ctx *s)
{
    if (s->separate_writeout)
        pthread_mutex_unlock(&s->lock);

    return 0;
}

int cyanrip_init_track_encoding(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                                cyanrip_track *t, enum cyanrip_output_formats format)
{
    int ret = 0;
    const cyanrip_out_fmt *cfmt = &crip_fmt_info[format];
    cyanrip_enc_ctx *s = av_mallocz(sizeof(*s));
    int deemphasis = (ctx->settings.deemphasis && t->preemphasis) || ctx->settings.force_deemphasis;

    const AVCodec *out_codec = NULL;

    s->t = t;
    s->ctx = ctx;
    s->cfmt = cfmt;
    s->separate_writeout = ctx->settings.enable_replaygain;
    atomic_init(&s->status, 0);
    atomic_init(&s->quit, 0);

    char *filename = crip_get_path(ctx, CRIP_PATH_TRACK, 1, cfmt, t);

    /* lavf init */
    ret = avformat_alloc_output_context2(&s->avf, NULL, cfmt->lavf_name, filename);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to init lavf context: %s!\n", av_err2str(ret));
        goto fail;
    }

    s->st_aud = avformat_new_stream(s->avf, NULL);
    if (!s->st_aud) {
        cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* Cover image init */
    CRIPArt *art = &t->art;
    if (!art->pkt) {
        int i;
        for (i = 0; i < ctx->nb_cover_arts; i++)
            if (!strcmp(dict_get(ctx->cover_arts[i].meta, "title"), "Front"))
                break;
        art = &ctx->cover_arts[i == ctx->nb_cover_arts ? 0 : i];
    }

    if (art->pkt && cfmt->coverart_supported &&
        !ctx->settings.disable_coverart_embedding) {
        s->st_img = avformat_new_stream(s->avf, NULL);
        if (!s->st_img) {
            cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        memcpy(s->st_img->codecpar, art->params, sizeof(AVCodecParameters));
        s->st_img->disposition |= AV_DISPOSITION_ATTACHED_PIC;
        s->st_img->time_base = (AVRational){ 1, 25 };
        av_dict_copy(&s->st_img->metadata, art->meta, 0);
        s->cover_art_pkt = av_packet_clone(art->pkt);
    }

    /* Find encoder */
    if (cfmt->codec == AV_CODEC_ID_NONE)
        out_codec = avcodec_find_encoder(ctx->settings.decode_hdcd ?
                                         AV_CODEC_ID_PCM_S32 :
                                         AV_CODEC_ID_PCM_S16);
    else
        out_codec = avcodec_find_encoder(cfmt->codec);

    if (!out_codec) {
        cyanrip_log(ctx, 0, "Codec not found (not compiled in lavc?)!\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* Output avctx */
    s->out_avctx = setup_out_avctx(ctx, s->avf, out_codec, cfmt,
                                   ctx->settings.decode_hdcd, deemphasis);
    if (!s->out_avctx) {
        cyanrip_log(ctx, 0, "Unable to init output avctx!\n");
        goto fail;
    }

    /* Set primary audio stream's parameters */
    s->st_aud->time_base = (AVRational){ 1, s->out_avctx->sample_rate };
    s->audio_stream_index = s->st_aud->index;

    /* Open encoder */
    ret = avcodec_open2(s->out_avctx, out_codec, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Could not open output codec context!\n");
        goto fail;
    }

    /* Set codecpar */
    ret = avcodec_parameters_from_context(s->st_aud->codecpar, s->out_avctx);
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

    /* SWR */
    s->swr = setup_init_swr(ctx, s->out_avctx,
                            ctx->settings.decode_hdcd, deemphasis);
    if (!s->swr)
        goto fail;

    /* FIFO */
    s->fifo = cr_frame_fifo_create(-1, FRAME_FIFO_BLOCK_NO_INPUT);
    if (!s->fifo)
        goto fail;

    /* Packet fifo */
    if (s->separate_writeout) {
        s->packet_fifo = cr_packet_fifo_create(-1, 0);
        if (!s->packet_fifo)
            goto fail;

        ret = pthread_mutex_init(&s->lock, NULL);
        if (ret != 0) {
            ret = AVERROR(ret);
            goto fail;
        }

        pthread_mutex_lock(&s->lock);
    } else {
        ret = open_output(ctx, s);
        if (ret < 0)
            goto fail;
    }

    av_freep(&filename);

    pthread_create(&s->thread, NULL, cyanrip_track_encoding, s);

    *enc_ctx = s;

    return 0;

fail:
    av_free(filename);
    cyanrip_end_track_encoding(&s);

    return ret;
}
