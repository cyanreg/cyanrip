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

#include "discid.h"
#include "cyanrip_log.h"

#include <libavutil/sha.h>
#include <libavutil/base64.h>
#include <libavutil/bprint.h>

int crip_fill_discid(cyanrip_ctx *ctx)
{
    struct AVSHA *sha_ctx = av_sha_alloc();
    int err = av_sha_init(sha_ctx, 160);
    if (err < 0) {
        cyanrip_log(ctx, 0, "Unable to init SHA for DiscID: %s!\n", av_err2str(err));
        av_free(sha_ctx);
        return err;
    }

    uint8_t temp[33];
    snprintf(temp, sizeof(temp), "%02X", ctx->tracks[0].number);
    av_sha_update(sha_ctx, temp, strlen(temp));

    int last_audio_track_idx = ctx->nb_cd_tracks - 1;
    for (; last_audio_track_idx >= 0; last_audio_track_idx--)
        if (!ctx->tracks[last_audio_track_idx].track_is_data)
            break;

    snprintf(temp, sizeof(temp), "%02X", ctx->tracks[last_audio_track_idx].number);
    av_sha_update(sha_ctx, temp, strlen(temp));

    lsn_t last = ctx->tracks[last_audio_track_idx].end_lsn + 151;
    snprintf(temp, sizeof(temp), "%08X", last);
    av_sha_update(sha_ctx, temp, strlen(temp));

    for (int i = 0; i < 99; i++) {
        uint32_t offset = 0;
        if (i <= last_audio_track_idx)
            offset = ctx->tracks[i].start_lsn + (i <= last_audio_track_idx)*150;
        snprintf(temp, sizeof(temp), "%08X", offset);
        av_sha_update(sha_ctx, temp, strlen(temp));
    }

    uint8_t digest[20];
    av_sha_final(sha_ctx, digest);
    av_free(sha_ctx);

    int discid_len = AV_BASE64_SIZE(20);
    char *discid = av_mallocz(discid_len);
    av_base64_encode(discid, discid_len, digest, 20);

    for (int i = 0; i < strlen(discid); i++) {
        if (discid[i] == '/') discid[i] = '_';
        if (discid[i] == '+') discid[i] = '.';
        if (discid[i] == '=') discid[i] = '-';
    }

    av_dict_set(&ctx->meta, "discid", discid, AV_DICT_DONT_STRDUP_VAL);

    /* FreeDB */
    uint32_t cddb = 0;
    for (int i = 0; i <= last_audio_track_idx; i++) {
        uint32_t m = (ctx->tracks[i].start_lsn + 150) / 75;
        while (m > 0) {
            cddb += m % 10;
            m /= 10;
        }
    }

    cddb  = (cddb % 0xff) << 24;
    cddb |= (last/75 - (ctx->tracks[0].start_lsn + 150)/75) << 8;
    cddb |= ctx->tracks[last_audio_track_idx].number;

    snprintf(temp, sizeof(temp), "%08x", cddb);

    av_dict_set(&ctx->meta, "cddb", temp, 0);

    /* TOC string */
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);

    av_bprintf(&buf, "https://musicbrainz.org/cdtoc/attach?toc=");

    av_bprintf(&buf, "%i%s%i%s%u", ctx->tracks[0].number, "+",
               ctx->tracks[last_audio_track_idx].number, "+", last);

    for (int i = 0; i <= last_audio_track_idx; i++) {
        uint32_t offset = ctx->tracks[i].start_lsn + (i <= last_audio_track_idx)*150;
        av_bprintf(&buf, "%s%u", "+", offset);
    }

    av_bprintf(&buf, "&tracks=%i", last_audio_track_idx + 1);
    av_bprintf(&buf, "&id=%s", discid);

    av_bprint_finalize(&buf, &ctx->mb_submission_url);

    return 0;
}
