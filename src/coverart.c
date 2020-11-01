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

#include <curl/curl.h>
#include <libavformat/avformat.h>
#include <libavutil/base64.h>

#include "coverart.h"
#include "cyanrip_log.h"

#define COVERART_DB_URL_BASE "http://coverartarchive.org/release"

int crip_save_art(cyanrip_ctx *ctx, CRIPArt *art, const cyanrip_out_fmt *fmt)
{
    int ret = 0;

    if (!art->pkt) {
        cyanrip_log(ctx, 0, "Cover art has no packet!\n");
        return 0;
    }

    char *filepath = crip_get_path(ctx, CRIP_PATH_COVERART, 1, fmt, art);
    if (!filepath)
        return 0;

    AVOutputFormat *ofm = av_guess_format(NULL, filepath, art->mime_type);
    AVFormatContext *avf = NULL;

    ret = avformat_alloc_output_context2(&avf, ofm, NULL, filepath);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to init lavf context: %s!\n", av_err2str(ret));
        goto fail;
    }

    AVStream *st = avformat_new_stream(avf, NULL);
    if (!st) {
        cyanrip_log(ctx, 0, "Unable to alloc stream!\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memcpy(st->codecpar, art->params, sizeof(AVCodecParameters));
    st->time_base = (AVRational){ 1, 25 };
    avf->oformat->video_codec = st->codecpar->codec_id;
    av_dict_copy(&st->metadata, art->meta, 0);
    av_dict_copy(&avf->metadata, art->meta, 0);

    /* Open for writing */
    ret = avio_open(&avf->pb, filepath, AVIO_FLAG_WRITE);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Couldn't open %s for writing: %s!\n", filepath, av_err2str(ret));
        goto fail;
    }
    av_freep(&filepath);

    /* Write header */
    ret = avformat_write_header(avf, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Couldn't write header: %s!\n", av_err2str(ret));
        goto fail;
    }

    AVPacket *pkt = av_packet_clone(art->pkt);
    pkt->stream_index = st->index;
    if ((ret = av_interleaved_write_frame(avf, pkt)) < 0) {
        cyanrip_log(ctx, 0, "Error writing picture packet: %s!\n", av_err2str(ret));
        goto fail;
    }
    av_packet_free(&pkt);

    if ((ret = av_write_trailer(avf)) < 0) {
        cyanrip_log(ctx, 0, "Error writing trailer: %s!\n", av_err2str(ret));
        goto fail;
    }

fail:
    if (avf)
        avio_closep(&avf->pb);
    avformat_free_context(avf);

    return ret;
}

void crip_free_art(CRIPArt *art)
{
    av_packet_free(&art->pkt);
    av_freep(&art->params);

    av_freep(&art->data);
    art->size = 0;

    av_freep(&art->source_url);
    av_freep(&art->mime_type);
    av_freep(&art->extension);

    av_dict_free(&art->meta);
}

static size_t receive_image(void *buffer, size_t size, size_t nb, void *opaque)
{
    CRIPArt *art = opaque;

    uint8_t *new_data = av_realloc(art->data, art->size + (size * nb));
    if (!new_data)
        return 0;

    memcpy(new_data + art->size, buffer, size * nb);

    art->data  = new_data;
    art->size += size * nb;

    return size * nb;
}

static int fetch_image(cyanrip_ctx *ctx, CURL *curl_ctx, CRIPArt *art,
                       const char *release_id, const char *type, int info_only,
                       const char *own_url)
{
    int ret;
    char errbuf[CURL_ERROR_SIZE];

    char temp[4096];
    if (!own_url) {
        snprintf(temp, sizeof(temp), "%s/%s/%s",
                 COVERART_DB_URL_BASE, release_id, type);
        curl_easy_setopt(curl_ctx, CURLOPT_URL, temp);
    } else {
        curl_easy_setopt(curl_ctx, CURLOPT_URL, own_url);
    }

    char user_agent[256] = { 0 };
    sprintf(user_agent, "cyanrip/%s ( https://github.com/cyanreg/cyanrip )", PROJECT_VERSION_STRING);
    curl_easy_setopt(curl_ctx, CURLOPT_USERAGENT, user_agent);

    curl_easy_setopt(curl_ctx, CURLOPT_WRITEFUNCTION, receive_image);
    curl_easy_setopt(curl_ctx, CURLOPT_WRITEDATA, art);

    curl_easy_setopt(curl_ctx, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl_ctx, CURLOPT_FAILONERROR, 1L); /* Explode on errors */

    if (!info_only) {
        cyanrip_log(ctx, 0, "Downloading %s cover art...\n", type);
        curl_easy_setopt(curl_ctx, CURLOPT_FOLLOWLOCATION, 1L);
    }

    CURLcode res = curl_easy_perform(curl_ctx);
    if (res != CURLE_OK) {
        /* Filter out the majority of 404 errors here */
        if (res == CURLE_HTTP_RETURNED_ERROR) {
            cyanrip_log(ctx, 0, "Unable to get cover art \"%s\": not found!\n", type);
            crip_free_art(art);
            ret = own_url ? AVERROR(EINVAL) : 0;
            goto end;
        }

        /* Different error */
        size_t len = strlen(errbuf);
        if (len)
            cyanrip_log(ctx, 0, "Unable to get cover art \"%s\": %s%s!\n",
                        type, errbuf, ((errbuf[len - 1] != '\n') ? "\n" : ""));
        else
            cyanrip_log(ctx, 0, "Unable to get cover art \"%s\": %s\n!\n",
                        type, curl_easy_strerror(res));
        ret = AVERROR(EINVAL);
        goto end;
    }

    /* Get content type, if not possible we probably have an error somewhere */
    char *content_type = NULL;
    res = curl_easy_getinfo(curl_ctx, CURLINFO_CONTENT_TYPE, &content_type);
    if (res != CURLE_OK) {
        cyanrip_log(ctx, 0, "Unable to get cover art \"%s\": %s\n!\n",
                    type, curl_easy_strerror(res));
        ret = AVERROR(EINVAL);
        goto end;
    }

    art->mime_type = av_strdup(content_type);

    /* Response code */
    long response_code = 0;
    res = curl_easy_getinfo(curl_ctx, CURLINFO_RESPONSE_CODE, &response_code);
    if (res != CURLE_OK) {
        cyanrip_log(ctx, 0, "Unable to get cover art \"%s\": %s\n!\n",
                    type, curl_easy_strerror(res));
        ret = AVERROR(EINVAL);
        goto end;
    }

    /* Get the URL */
    char *final_url = NULL;
    res = curl_easy_getinfo(curl_ctx, CURLINFO_EFFECTIVE_URL, &final_url);
    if (res != CURLE_OK) {
        cyanrip_log(ctx, 0, "Unable to get cover art \"%s\": %s\n!\n",
                    type, curl_easy_strerror(res));
        ret = AVERROR(EINVAL);
        goto end;
    }

    ret = art->size;

    if (info_only) {
        av_freep(&art->data);
        art->size = 0;
    } else {
        char header[99];
        snprintf(header, sizeof(header), "data:%s;base64,", art->mime_type);

        size_t data_len = AV_BASE64_SIZE(art->size);
        char *data = av_mallocz(strlen(header) + data_len);
        memcpy(data, header, strlen(header));

        av_base64_encode(data + strlen(header), data_len, art->data, art->size);

        av_free(art->data);
        art->data = data;
        art->size = data_len;
    }

    art->source_url = av_strdup(final_url);

end:
    if (res < 0)
        crip_free_art(art);

    return ret;
}

static int demux_image(cyanrip_ctx *ctx, CRIPArt *art, int info_only)
{
    int ret = 0;
    AVFormatContext *avf = NULL;
    const char *in_url = art->data ? (const char *)art->data : art->source_url;

    ret = avformat_open_input(&avf, in_url, NULL, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to open \"%s\": %s!\n", art->data ? "(data)" : in_url,
                    av_err2str(ret));
        goto end;
    }

    ret = avformat_find_stream_info(avf, NULL);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Unable to get cover image info: %s!\n", av_err2str(ret));
        goto end;
    }

    art->params = av_calloc(1, sizeof(AVCodecParameters));
    if (!art->params)
        goto end;

    memcpy(art->params, avf->streams[0]->codecpar, sizeof(AVCodecParameters));

    /* Output extension guesswork */
    av_freep(&art->extension);
    if (art->params->codec_id == AV_CODEC_ID_MJPEG) {
        art->extension = av_strdup("jpg");
    } else if (art->params->codec_id == AV_CODEC_ID_PNG) {
        art->extension = av_strdup("png");
    } else if (art->params->codec_id == AV_CODEC_ID_BMP) {
        art->extension = av_strdup("bmp");
    } else if (art->params->codec_id == AV_CODEC_ID_TIFF) {
        art->extension = av_strdup("tiff");
    } else if (art->params->codec_id == AV_CODEC_ID_AV1) {
        art->extension = av_strdup("avif");
    } else if (art->params->codec_id == AV_CODEC_ID_HEVC) {
        art->extension = av_strdup("heif");
    } else if (art->params->codec_id == AV_CODEC_ID_WEBP) {
        art->extension = av_strdup("webp");
    } else if (art->params->codec_id != AV_CODEC_ID_NONE) {
        art->extension = av_strdup(avcodec_get_name(art->params->codec_id));
    } else {
        ret = AVERROR(EINVAL);
        cyanrip_log(ctx, 0, "Error demuxing cover image: %s!\n", av_err2str(ret));
        goto end;
    }

    if (info_only)
        goto end;

    art->pkt = av_packet_alloc();
    if (!art->pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = av_read_frame(avf, art->pkt);
    if (ret < 0) {
        cyanrip_log(ctx, 0, "Error demuxing cover image: %s!\n", av_err2str(ret));
        goto end;
    }

    avformat_close_input(&avf);

    av_freep(&art->data);
    art->size = 0;

    return 0;

end:
    avformat_close_input(&avf);

    return ret;
}

static int string_is_url(const char *src)
{
    return !strncmp(src, "http://", 4)   ||
           !strncmp(src, "https://", 4)  ||
           !strncmp(src, "ftp://", 4)    ||
           !strncmp(src, "ftps://", 4)   ||
           !strncmp(src, "sftp://", 4)   ||
           !strncmp(src, "tftp://", 4)   ||
           !strncmp(src, "gopher://", 4) ||
           !strncmp(src, "telnet://", 4);
}

int crip_fill_coverart(cyanrip_ctx *ctx, int info_only)
{
    int err = 0;

    int have_front = 0;
    int have_back = 0;
    for (int i = 0; i < ctx->nb_cover_arts; i++) {
        const char *title = dict_get(ctx->cover_arts[i].meta, "title");
        have_front |= !strcmp(title, "Front");
        have_back |= !strcmp(title, "Back");
    }

    CURL *curl_ctx = curl_easy_init();

    if (!have_front || !have_back) {
        const char *release_id = dict_get(ctx->meta, "release_id");
        if (!release_id && !ctx->settings.disable_coverart_db) {
            cyanrip_log(ctx, 0, "Release ID unavailable, cannot search Cover Art DB!\n");
        } else if (!ctx->settings.disable_coverart_db) {
            int has_err = 0;
            if (!have_front) {
                has_err = fetch_image(ctx, curl_ctx, &ctx->cover_arts[ctx->nb_cover_arts], release_id, "front", info_only, NULL);
                if (has_err > 0) {
                    av_dict_set(&ctx->cover_arts[ctx->nb_cover_arts].meta, "title", "Front", 0);
                    av_dict_set(&ctx->cover_arts[ctx->nb_cover_arts].meta, "source", "Cover Art DB", 0);
                    ctx->cover_arts[ctx->nb_cover_arts].extension = av_strdup("jpg");
                    ctx->nb_cover_arts++;
                }
            }
            if (!have_back && (has_err > 0)) {
                has_err = fetch_image(ctx, curl_ctx, &ctx->cover_arts[ctx->nb_cover_arts], release_id, "back", info_only, NULL);
                if (has_err > 0) {
                    av_dict_set(&ctx->cover_arts[ctx->nb_cover_arts].meta, "title", "Back", 0);
                    av_dict_set(&ctx->cover_arts[ctx->nb_cover_arts].meta, "source", "Cover Art DB", 0);
                    ctx->cover_arts[ctx->nb_cover_arts].extension = av_strdup("jpg");
                    ctx->nb_cover_arts++;
                }
            }
        }
    }

    for (int i = 0; i < ctx->nb_cover_arts; i++) {
        char *source_url = ctx->cover_arts[i].source_url;
        const char *title = dict_get(ctx->cover_arts[i].meta, "title");
        int is_url = string_is_url(source_url);

        /* If its a url, we haven't downloaded it from CADB and we're not info printing */
        if (is_url && !ctx->cover_arts[i].data && !info_only) {
            err = fetch_image(ctx, curl_ctx, &ctx->cover_arts[i], NULL, title, 0, source_url);
            if (err < 0)
                goto end;
        }

        /* If we don't have to download it, or we have data */
        if (!is_url || !info_only) {
            if ((err = demux_image(ctx, &ctx->cover_arts[i], info_only)) < 0)
                goto end;
        }
    }

end:
    curl_easy_cleanup(curl_ctx);

    return err;
}

int crip_fill_track_coverart(cyanrip_ctx *ctx, int info_only)
{
    int err = 0;
    CURL *curl_ctx = curl_easy_init();

    for (int i = 0; i < ctx->nb_tracks; i++) {
        if (!ctx->tracks[i].art.source_url)
            continue;

        char *source_url = ctx->tracks[i].art.source_url;
        int is_url = string_is_url(source_url);

        /* If its a url, we haven't downloaded it from CADB and we're not info printing */
        if (is_url && !ctx->tracks[i].art.data && !info_only) {
            err = fetch_image(ctx, curl_ctx, &ctx->tracks[i].art, NULL, "track", 0, source_url);
            if (err < 0)
                goto end;
        }

        /* If we don't have to download it, or we have data */
        if (!is_url || !info_only) {
            if ((err = demux_image(ctx, &ctx->tracks[i].art, info_only)) < 0)
                goto end;
        }
    }

end:
    curl_easy_cleanup(curl_ctx);

    return err;
}
