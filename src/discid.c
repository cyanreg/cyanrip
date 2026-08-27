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
    /* Follows libdiscid's mb_disc_load_toc() and create_disc_id(). The TOC
     * is indexed by track number rather than by position, entry 0 is the
     * lead-out, and every offset is in frames since the lead-in. */
    int32_t offsets[100] = { 0 };
    lsn_t track_lsn[100] = { 0 };
    const int nb_offsets = sizeof(offsets) / sizeof(*offsets);

    for (int i = 0; i < ctx->nb_cd_tracks; i++) {
        cyanrip_track *t = &ctx->tracks[i];
        if ((t->number > 0) && (t->number < nb_offsets))
            track_lsn[t->number] = t->start_lsn;
    }

    /* Data tracks at the front and in between audio tracks are a part of
     * the disc, MusicBrainz expects a release to start at track 1. Only
     * the tracks past the last audio one are left out, the lead-out takes
     * their place. */
    int first_track = ctx->tracks[0].number;
    int last_track = 0;
    for (int i = 0; i < ctx->nb_cd_tracks; i++)
        if (!ctx->tracks[i].track_is_data)
            last_track = ctx->tracks[i].number;

    if (!last_track) {
        cyanrip_log(ctx, 0, "Disc has no audio tracks, skipping DiscID generation!\n");
        return 0;
    }

    for (int i = first_track; i <= last_track; i++)
        offsets[i] = track_lsn[i] > 0 ? track_lsn[i] + 150 : 150;

    offsets[0] = ctx->id_leadout_lsn + 150;

    /* A lead-out landing before the start of the last track means the TOC
     * is lying about that track, which is what copy protection schemes do.
     * Drop it and let the track before it be the last one. */
    while ((last_track > first_track) && (offsets[0] < offsets[last_track])) {
        offsets[last_track--] = 0;
        offsets[0] = track_lsn[last_track + 1] - CRIP_XA_INTERVAL + 150;
    }

    if (offsets[0] <= offsets[last_track]) {
        cyanrip_log(ctx, 0, "Disc TOC is invalid, skipping DiscID generation!\n");
        return 0;
    }

    struct AVSHA *sha_ctx = av_sha_alloc();
    int err = av_sha_init(sha_ctx, 160);
    if (err < 0) {
        cyanrip_log(ctx, 0, "Unable to init SHA for DiscID: %s!\n", av_err2str(err));
        av_free(sha_ctx);
        return err;
    }

    uint8_t temp[33];
    snprintf(temp, sizeof(temp), "%02X", first_track);
    av_sha_update(sha_ctx, temp, strlen(temp));

    snprintf(temp, sizeof(temp), "%02X", last_track);
    av_sha_update(sha_ctx, temp, strlen(temp));

    for (int i = 0; i < nb_offsets; i++) {
        snprintf(temp, sizeof(temp), "%08X", (uint32_t)offsets[i]);
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

    av_dict_set(&ctx->meta, "musicbrainz_discid", discid, AV_DICT_DONT_STRDUP_VAL);

    /* FreeDB */
    uint32_t cddb = 0;
    for (int i = 1; i <= last_track; i++) {
        uint32_t m = offsets[i] / 75;
        while (m > 0) {
            cddb += m % 10;
            m /= 10;
        }
    }

    cddb  = (cddb % 0xff) << 24;
    cddb |= (offsets[0]/75 - offsets[1]/75) << 8;
    cddb |= last_track;

    snprintf(temp, sizeof(temp), "%08X", cddb);

    av_dict_set(&ctx->meta, "cddb", temp, 0);

    /* TOC string */
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);

    av_bprintf(&buf, "https://musicbrainz.org/cdtoc/attach?toc=");

    av_bprintf(&buf, "%i%s%i%s%u", first_track, "+", last_track, "+", offsets[0]);

    for (int i = first_track; i <= last_track; i++)
        av_bprintf(&buf, "%s%u", "+", offsets[i]);

    av_bprintf(&buf, "&tracks=%i", last_track);
    av_bprintf(&buf, "&id=%s", discid);

    av_bprint_finalize(&buf, &ctx->mb_submission_url);

    return 0;
}
