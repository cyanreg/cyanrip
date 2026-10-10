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

#include "../config.h"
#include "acoustid.h"

int crip_acoustid_enabled(cyanrip_ctx *ctx)
{
#ifdef HAVE_CHROMAPRINT
    return !ctx->settings.disable_acoustid;
#else
    return 0;
#endif
}

#ifdef HAVE_CHROMAPRINT

#include <stdlib.h>
#include <chromaprint.h>
#include <curl/curl.h>
#include <libavutil/bprint.h>

#include "accurip.h"
#include "cyanrip_log.h"
#include "json.h"

/* cyanrip's own application key. Submissions also carry the user's key. */
#define ACOUSTID_CLIENT_KEY "alPXffSjt8"
#define ACOUSTID_SUBMIT_URL "https://api.acoustid.org/v2/submit"

/* As much audio as fpcalc fingerprints by default, which is what Picard
 * runs it with and submits */
#define FINGERPRINT_SECONDS 120

/* Picard doesn't link a fingerprint to a recording whose length differs
 * from the file's by more than this */
#define MAX_LENGTH_DIFF_MS 30000

struct CRIPFingerprint {
    ChromaprintContext *cp;
    int samples_left; /* Samples of the current pass still to fingerprint */
};

int crip_init_fingerprint(cyanrip_ctx *ctx)
{
    if (!crip_acoustid_enabled(ctx))
        return 0;

    CRIPFingerprint *s = ctx->fingerprint;
    if (!s) {
        s = av_mallocz(sizeof(*s));
        if (!s)
            return AVERROR(ENOMEM);

        s->cp = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
        if (!s->cp) {
            av_free(s);
            return AVERROR(ENOMEM);
        }

        ctx->fingerprint = s;
    }

    if (!chromaprint_start(s->cp, 44100, 2))
        return AVERROR_EXTERNAL;

    s->samples_left = FINGERPRINT_SECONDS * 44100 * 2;

    return 0;
}

void crip_process_fingerprint(cyanrip_ctx *ctx, const uint8_t *data, int bytes)
{
    CRIPFingerprint *s = ctx->fingerprint;
    int16_t buf[CDIO_CD_FRAMESIZE_RAW >> 1];

    if (!s)
        return;

    /* Native order, as the encoders take it, copied out for alignment */
    int samples = FFMIN(bytes >> 1, s->samples_left);
    while (samples > 0) {
        int len = FFMIN(samples, FF_ARRAY_ELEMS(buf));
        memcpy(buf, data, len * sizeof(*buf));
        chromaprint_feed(s->cp, buf, len);

        data += len * sizeof(*buf);
        samples -= len;
        s->samples_left -= len;
    }
}

void crip_finalize_fingerprint(cyanrip_ctx *ctx, cyanrip_track *t)
{
    CRIPFingerprint *s = ctx->fingerprint;
    char *fp = NULL;
    int size = 0;

    if (!s)
        return;

    av_freep(&t->acoustid_fingerprint);

    /* Tracks too short to fingerprint still encode to a header, AQAAAA */
    if (chromaprint_finish(s->cp) && chromaprint_get_raw_fingerprint_size(s->cp, &size) &&
        size > 0 && chromaprint_get_fingerprint(s->cp, &fp))
        t->acoustid_fingerprint = av_strdup(fp);

    chromaprint_dealloc(fp);
}

void crip_free_fingerprint(cyanrip_ctx *ctx)
{
    if (!ctx->fingerprint)
        return;

    chromaprint_free(ctx->fingerprint->cp);
    av_freep(&ctx->fingerprint);
}

/* Why the fingerprints can't be submitted, NULL if they can */
static const char *submission_blocker(cyanrip_ctx *ctx)
{
    if (ctx->settings.disable_accurip)
        return "AccurateRip disabled";
    if (ctx->ar_db_status != CYANRIP_ACCUDB_FOUND)
        return "disc not in the AccurateRip database";
    if (ctx->settings.disable_mb)
        return "MusicBrainz disabled";

    for (int i = 0; i < ctx->nb_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];
        if (t->track_is_data)
            continue;

        if (!t->computed_crcs)
            return "not every track was ripped";
        if (!t->acoustid_fingerprint)
            return "not every track could be fingerprinted";
        if (crip_find_ar(t, t->acurip_checksum_v1, 0) < 1 &&
            crip_find_ar(t, t->acurip_checksum_v2, 0) < 1)
            return "not every track was ripped accurately";
        if (!dict_get(t->meta, "mbid") || !t->mb_length_ms)
            return "not every track has a MusicBrainz recording";

        int64_t length_ms = (int64_t)t->nb_samples * 1000 / 44100;
        if (llabs(length_ms - t->mb_length_ms) > MAX_LENGTH_DIFF_MS)
            return "a track's length differs from its MusicBrainz recording's";
    }

    if (!ctx->settings.acoustid_key)
        return "no AcoustID user API key given via -k";

    return NULL;
}

typedef struct RecvCtx {
    char *data;
    size_t size;
} RecvCtx;

static size_t receive_data(void *buffer, size_t size, size_t nb, void *opaque)
{
    RecvCtx *rctx = opaque;

    char *data = av_realloc(rctx->data, rctx->size + (size * nb));
    if (!data)
        return 0;

    memcpy(data + rctx->size, buffer, size * nb);
    rctx->data = data;
    rctx->size += size * nb;

    return size * nb;
}

/* idx < 0 for parameters which aren't per fingerprint */
static int add_param(AVBPrint *bp, CURL *curl_ctx, const char *name, int idx,
                     const char *val)
{
    if (!val)
        return 0;

    char *esc = curl_easy_escape(curl_ctx, val, 0);
    if (!esc)
        return AVERROR(ENOMEM);

    if (idx < 0)
        av_bprintf(bp, "%s%s=%s", bp->len ? "&" : "", name, esc);
    else
        av_bprintf(bp, "&%s.%i=%s", name, idx, esc);

    curl_free(esc);

    return av_bprint_is_complete(bp) ? 0 : AVERROR(ENOMEM);
}

/* Returns 0 once AcoustID has accepted the fingerprints */
static int submit_fingerprints(cyanrip_ctx *ctx)
{
    int ret = 0, nb = 0;
    char errbuf[CURL_ERROR_SIZE] = { 0 };
    RecvCtx rctx = { 0 };
    char *status = NULL, *msg = NULL;
    AVBPrint body;

    av_bprint_init(&body, 0, AV_BPRINT_SIZE_UNLIMITED);

    CURL *curl_ctx = curl_easy_init();
    if (!curl_ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret |= add_param(&body, curl_ctx, "client", -1, ACOUSTID_CLIENT_KEY);
    ret |= add_param(&body, curl_ctx, "clientversion", -1, PROJECT_VERSION_STRING);
    ret |= add_param(&body, curl_ctx, "user", -1, ctx->settings.acoustid_key);
    ret |= add_param(&body, curl_ctx, "format", -1, "json");

    for (int i = 0; i < ctx->nb_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];
        if (t->track_is_data)
            continue;

        char duration[32];
        snprintf(duration, sizeof(duration), "%zu", (t->nb_samples + 22050) / 44100);

        ret |= add_param(&body, curl_ctx, "duration", nb, duration);
        ret |= add_param(&body, curl_ctx, "fingerprint", nb, t->acoustid_fingerprint);
        ret |= add_param(&body, curl_ctx, "mbid", nb, dict_get(t->meta, "mbid"));
        nb++;
    }

    if (ret < 0) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    char user_agent[256];
    snprintf(user_agent, sizeof(user_agent), "cyanrip/%s ( https://github.com/cyanreg/cyanrip )",
             PROJECT_VERSION_STRING);

    curl_easy_setopt(curl_ctx, CURLOPT_URL, ACOUSTID_SUBMIT_URL);
    curl_easy_setopt(curl_ctx, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl_ctx, CURLOPT_POSTFIELDS, body.str);
    curl_easy_setopt(curl_ctx, CURLOPT_POSTFIELDSIZE, (long)body.len);
    curl_easy_setopt(curl_ctx, CURLOPT_WRITEFUNCTION, receive_data);
    curl_easy_setopt(curl_ctx, CURLOPT_WRITEDATA, &rctx);
    curl_easy_setopt(curl_ctx, CURLOPT_ERRORBUFFER, errbuf);

    /* The files are only written once this returns */
    curl_easy_setopt(curl_ctx, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl_ctx);
    if (res != CURLE_OK) {
        size_t len = strlen(errbuf);
        if (len && errbuf[len - 1] == '\n')
            errbuf[len - 1] = '\0';
        cyanrip_log(ctx, 0, "AcoustID submission failed: %s\n",
                    len ? errbuf : curl_easy_strerror(res));
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    long code = 0;
    curl_easy_getinfo(curl_ctx, CURLINFO_RESPONSE_CODE, &code);

    const char *data = rctx.data;
    const char *data_end = data ? data + rctx.size : NULL;

    status = crip_json_str(crip_json_get(data, data_end, "status"), data_end);
    if (code == 200 && status && !strcmp(status, "ok")) {
        int nb_accepted = crip_json_count(crip_json_get(data, data_end, "submissions"),
                                          data_end);
        cyanrip_log(ctx, 0, "AcoustID: submitted %i fingerprints, pending import\n",
                    nb_accepted < 0 ? nb : nb_accepted);
        goto end;
    }

    /* Errors come as {"status": "error", "error": {"code": N, "message": "..."}} */
    msg = crip_json_str(crip_json_get(crip_json_get(data, data_end, "error"),
                                      data_end, "message"), data_end);
    if (msg)
        cyanrip_log(ctx, 0, "AcoustID submission failed: %s\n", msg);
    else
        cyanrip_log(ctx, 0, "AcoustID submission failed: unexpected reply (HTTP %li)\n", code);
    ret = AVERROR_EXTERNAL;

end:
    av_free(msg);
    av_free(status);
    av_free(rctx.data);
    av_bprint_finalize(&body, NULL);
    curl_easy_cleanup(curl_ctx);

    return ret;
}

void crip_acoustid_finish(cyanrip_ctx *ctx)
{
    if (!ctx->fingerprint || quit_now)
        return;

    const char *reason = submission_blocker(ctx);
    if (!reason) {
        if (!submit_fingerprints(ctx))
            return;
        reason = "submission failed";
    }

    /* Left for a tagger to submit, Picard reuses this tag rather than
     * recomputing the fingerprint */
    int nb = 0;
    for (int i = 0; i < ctx->nb_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];
        if (!t->acoustid_fingerprint)
            continue;

        av_dict_set(&t->meta, "acoustid_fingerprint", t->acoustid_fingerprint, 0);
        nb++;
    }

    if (nb)
        cyanrip_log(ctx, 0, "AcoustID: %i fingerprints saved to tags, not submitted: %s\n",
                    nb, reason);
}

#else

int crip_init_fingerprint(cyanrip_ctx *ctx)
{
    return 0;
}

void crip_process_fingerprint(cyanrip_ctx *ctx, const uint8_t *data, int bytes)
{
}

void crip_finalize_fingerprint(cyanrip_ctx *ctx, cyanrip_track *t)
{
}

void crip_acoustid_finish(cyanrip_ctx *ctx)
{
}

void crip_free_fingerprint(cyanrip_ctx *ctx)
{
}

#endif
