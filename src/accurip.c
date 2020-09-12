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

#include <accurip.h>
#include "cyanrip_log.h"
#include "bytestream.h"

#include <curl/curl.h>

#define ACCURIP_DB_BASE_URL "http://www.accuraterip.com/accuraterip"

static int get_accurip_ids(cyanrip_ctx *ctx, uint32_t *id_type_1, uint32_t *id_type_2)
{
    int audio_tracks = 0;
    uint64_t idt1 = 0x0;
    uint64_t idt2 = 0x0;

    for (int i = 0; i < ctx->nb_cd_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];
        if (t->track_is_data)
            continue;

        idt1 += t->start_lsn;
        idt2 += (t->start_lsn ? t->start_lsn : 1) * t->number;
        audio_tracks++;
    }

    lsn_t last = ctx->tracks[audio_tracks - 1].end_lsn + 1;
    idt1 += last;
    idt2 += last * (audio_tracks + 1);

    idt1 &= 0xffffffff;
    idt2 &= 0xffffffff;

    *id_type_1 = idt1;
    *id_type_2 = idt2;

    return audio_tracks;
}

typedef struct RecvCtx {
    cyanrip_ctx *ctx;
    uint8_t *data;
    size_t size;
} RecvCtx;

static size_t receive_data(void *buffer, size_t size, size_t nb, void *opaque)
{
    RecvCtx *rctx = opaque;

    rctx->data = av_realloc(rctx->data, rctx->size + (size * nb));
    memcpy(rctx->data + rctx->size, buffer, size * nb);
    rctx->size += size * nb;

    return size * nb;
}

int crip_fill_accurip(cyanrip_ctx *ctx)
{
    int ret = 0;
    char errbuf[CURL_ERROR_SIZE];
    RecvCtx rctx = { .ctx = ctx };

    if (ctx->settings.disable_accurip)
        return 0;

    CURL *curl_ctx = curl_easy_init();

    /* Get both accurip disc IDs */
    uint32_t id_type_1, id_type_2;
    int audio_tracks = get_accurip_ids(ctx, &id_type_1, &id_type_2);

    /* Get CDDB ID */
    const char *cddb_id_str = dict_get(ctx->meta, "cddb");
    if (!cddb_id_str) {
        cyanrip_log(ctx, 0, "Unable to get AccuRIP DB data: missing CDDB ID!\n");
        goto end;
    }

    /* Format all the data in the needed way */
    uint32_t cddb_id = strtol(cddb_id_str, NULL, 16);
    char id_type_1_s[9] = { 0 };
    snprintf(id_type_1_s, sizeof(id_type_1_s), "%08x", id_type_1);

    /* Finally compose the URL */
    char request_url[512] = { 0 };
    snprintf(request_url, sizeof(request_url), "%s/%c/%c/%c/dBAR-%.3d-%s-%08x-%08x.bin",
             ACCURIP_DB_BASE_URL,
             id_type_1_s[7], id_type_1_s[6], id_type_1_s[5],
             audio_tracks, id_type_1_s, id_type_2, cddb_id);

    curl_easy_setopt(curl_ctx, CURLOPT_URL, request_url);

    char user_agent[256] = { 0 };
    sprintf(user_agent, "cyanrip/%s ( https://github.com/cyanreg/cyanrip )", PROJECT_VERSION_STRING);
    curl_easy_setopt(curl_ctx, CURLOPT_USERAGENT, user_agent);

    curl_easy_setopt(curl_ctx, CURLOPT_WRITEFUNCTION, receive_data);
    curl_easy_setopt(curl_ctx, CURLOPT_WRITEDATA, &rctx);

    curl_easy_setopt(curl_ctx, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl_ctx, CURLOPT_FAILONERROR, 1L); /* Explode on errors */

    CURLcode res = curl_easy_perform(curl_ctx);
    if (res != CURLE_OK) {
        /* Filter out the majority of 404 errors here */
        if (res == CURLE_HTTP_RETURNED_ERROR) {
            cyanrip_log(ctx, 0, "Unable to get AccuRIP DB data: missing entry!\n");
            ctx->ar_db_status = CYANRIP_ACCUDB_NOT_FOUND;
            goto end;
        }

        /* Different error */
        size_t len = strlen(errbuf);
        if (len)
            cyanrip_log(ctx, 0, "Unable to get AccuRIP DB data: %s%s!\n",
                        errbuf, ((errbuf[len - 1] != '\n') ? "\n" : ""));
        else
            cyanrip_log(ctx, 0, "Unable to get AccuRIP DB data: %s\n!\n",
                        curl_easy_strerror(res));
        ctx->ar_db_status = CYANRIP_ACCUDB_ERROR;
        goto end;
    }

    /* Get content type, if not possible we probably have an error somewhere */
    char *content_type = NULL;
    res = curl_easy_getinfo(curl_ctx, CURLINFO_CONTENT_TYPE, &content_type);
    if (res != CURLE_OK) {
        cyanrip_log(ctx, 0, "Unable to get AccuRIP DB data: %s\n!\n",
                    curl_easy_strerror(res));
        goto end;
    }

    /* If we have a binary we're pretty sure we've found a match */
    if (strcmp(content_type, "application/octet-stream")) {
        /* Atrocious heuristics to determine whether we have an error or binary data, don't look */
        char *html_loc = strstr((const char *)rctx.data, "html");
        if (html_loc && (html_loc - (char *)rctx.data) < 64) {
            /* If we have "html" in the first 64 bytes its likely an error.
             * This is painful to write. */
            cyanrip_log(ctx, 0, "Unable to get AccuRIP DB data: missing entry!\n");
            ctx->ar_db_status = CYANRIP_ACCUDB_NOT_FOUND;
            goto end;
        }
    }

    GetByteContext gbc = { 0 };
    bytestream2_init(&gbc, rctx.data, rctx.size);

    if (bytestream2_get_byte(&gbc) != audio_tracks ||
        bytestream2_get_le32(&gbc) != id_type_1 ||
        bytestream2_get_le32(&gbc) != id_type_2 ||
        bytestream2_get_le32(&gbc) != cddb_id) {
        cyanrip_log(ctx, 0, "AccuRIP DB error: mismatching entry found!\n");
        ctx->ar_db_status = CYANRIP_ACCUDB_MISMATCH;
        goto end;
    }

    ctx->ar_db_status = CYANRIP_ACCUDB_FOUND;

    for (int i = 0; i < ctx->nb_cd_tracks; i++) {
        if (ctx->tracks[i].track_is_data)
            continue;

        ctx->tracks[i].ar_db_status = CYANRIP_ACCUDB_FOUND;
        ctx->tracks[i].ar_db_confidence = bytestream2_get_byte(&gbc);
        ctx->tracks[i].ar_db_checksum = bytestream2_get_le32(&gbc);
        ctx->tracks[i].ar_db_checksum_450 = bytestream2_get_le32(&gbc);
    }

end:
    curl_easy_cleanup(curl_ctx);
    av_free(rctx.data);

    return ret;
}
