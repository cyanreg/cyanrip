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

#include "musicbrainz.h"
#include "cyanrip_log.h"

#include <musicbrainz5/mb5_c.h>

#define READ_MB(FUNC, MBCTX, DICT, KEY)                                        \
    do {                                                                       \
        int len = FUNC(MBCTX, NULL, 0) + 1;                                    \
        char *str = av_mallocz(4*len);                                         \
        FUNC(MBCTX, str, len);                                                 \
        av_dict_set(&DICT, KEY, str, AV_DICT_DONT_STRDUP_VAL | AV_DICT_APPEND);\
    } while (0)

static void mb_credit(Mb5ArtistCredit credit, AVDictionary *dict, const char *key)
{
    Mb5NameCreditList namecredit_list = mb5_artistcredit_get_namecreditlist(credit);
    for (int i = 0; i < mb5_namecredit_list_size(namecredit_list); i++) {
        Mb5NameCredit namecredit = mb5_namecredit_list_item(namecredit_list, i);

        if (mb5_namecredit_get_name(namecredit, NULL, 0)) {
            READ_MB(mb5_namecredit_get_name, namecredit, dict, key);
        } else {
            Mb5Artist artist = mb5_namecredit_get_artist(namecredit);
            if (artist)
                READ_MB(mb5_artist_get_name, artist, dict, key);
        }

        READ_MB(mb5_namecredit_get_joinphrase, namecredit, dict, key);
    }
}

static int mb_tracks(cyanrip_ctx *ctx, Mb5Release release, const char *discid, int discnumber)
{
    /* Set totaldiscs if possible */
    Mb5MediumList medium_list = mb5_release_get_mediumlist(release);
    int num_cds = mb5_medium_list_size(medium_list);
    av_dict_set_int(&ctx->meta, "totaldiscs", num_cds, 0);

    if (num_cds == 1 && !discnumber)
        av_dict_set_int(&ctx->meta, "disc", 1, 0);

    int media_idx;
    if (discnumber) {
        if (discnumber < 1 || discnumber > num_cds) {
            cyanrip_log(ctx, 0, "Invalid disc number %i, release only has %i CDs\n", discnumber, num_cds);
            return 1;
        }
        medium_list = mb5_release_get_mediumlist(release);
        media_idx = discnumber - 1;
    } else {
        medium_list = mb5_release_media_matching_discid(release, discid);
        if (!medium_list) {
            cyanrip_log(ctx, 0, "No mediums matching DiscID.\n");
            return 0;
        }
        media_idx = 0;
    }

    Mb5Medium medium = mb5_medium_list_item(medium_list, media_idx);
    if (!medium) {
        cyanrip_log(ctx, 0, "Got empty medium list.\n");
        if (discnumber)
            return 1;
        goto end;
    }

    Mb5TrackList track_list = mb5_medium_get_tracklist(medium);
    if (!track_list) {
        cyanrip_log(ctx, 0, "Medium has no track list.\n");
        goto end;
    }

    for (int i = 0; i < mb5_track_list_size(track_list); i++) {
        if (i >= ctx->nb_tracks)
            break;
        Mb5Track track = mb5_track_list_item(track_list, i);
        Mb5Recording recording = mb5_track_get_recording(track);
        Mb5ArtistCredit credit;
        if (recording) {
            READ_MB(mb5_recording_get_title, recording, ctx->tracks[i].meta, "title");
            credit = mb5_recording_get_artistcredit(recording);
        } else {
            READ_MB(mb5_track_get_title, track, ctx->tracks[i].meta, "title");
            credit = mb5_track_get_artistcredit(track);
        }
        if (credit)
            mb_credit(credit, ctx->tracks[i].meta, "artist");
    }

end:
    return 0;
}

static int mb_metadata(cyanrip_ctx *ctx, int manual_metadata_specified, int release_idx, char *release_str, int discnumber)
{
    int ret = 0;
    Mb5Query query = mb5_query_new("cyanrip", NULL, 0);
    if (!query) {
        cyanrip_log(ctx, 0, "Could not connect to MusicBrainz.\n");
        return 1;
    }

    char *names[] = { "inc" };
    char *values[] = { "recordings artist-credits" };
    const char *discid = dict_get(ctx->meta, "discid");
    Mb5Metadata metadata = mb5_query_query(query, "discid", discid, 0, 1, names, values);
    if (!metadata) {
        tQueryResult res = mb5_query_get_lastresult(query);
        if (res != eQuery_ResourceNotFound) {
            int chars = mb5_query_get_lasterrormessage(query, NULL, 0) + 1;
            char *msg = av_mallocz(chars*sizeof(*msg));
            mb5_query_get_lasterrormessage(query, msg, chars);
            cyanrip_log(ctx, 0, "MusicBrainz lookup failed: %s\n", msg);
            av_freep(&msg);
        }

        switch(res) {
        case eQuery_Timeout:
        case eQuery_ConnectionError:
            cyanrip_log(ctx, 0, "Connection failed, try again? Or disable via -n\n");
            break;
        case eQuery_AuthenticationError:
        case eQuery_FetchError:
        case eQuery_RequestError:
            cyanrip_log(ctx, 0, "Error fetching/requesting/auth, this shouldn't happen.\n");
            break;
        case eQuery_ResourceNotFound:
            if (manual_metadata_specified) {
                cyanrip_log(ctx, 0, "Unable to find metadata for this CD, but "
                            "metadata has been manually specified, continuing.\n");
                goto end;
            } else {
                cyanrip_log(ctx, 0, "Unable to find release info for this CD, "
                            "and metadata hasn't been manually added!\n");
                cyanrip_log(ctx, 0, "To continue add metadata via -a or -t, or ignore via -n!\n");
            }
            break;
        default:
            break;
        }

        ret = 1;
        goto end;
    }

    Mb5ReleaseList release_list = NULL;
    Mb5Disc disc = mb5_metadata_get_disc(metadata);
    if (!disc) {
        cyanrip_log(ctx, 0, "DiscID not found in MusicBrainz\n");
        goto end_meta;
    }

    release_list = mb5_disc_get_releaselist(disc);
    if (!release_list) {
        cyanrip_log(ctx, 0, "DiscID has no associated releases.\n");
        goto end_meta;
    }

    int num_releases = mb5_release_list_size(release_list);
    if (!num_releases) {
        cyanrip_log(ctx, 0, "No releases found for DiscID.\n");
        goto end_meta;
    } else if (num_releases > 1 && ((release_idx < 0) && !release_str)) {
        cyanrip_log(ctx, 0, "Multiple releases found in database for discid:\n");
        for (int i = 0; i < num_releases; i++) {
            Mb5Release tmp_rel = mb5_release_list_item(release_list, i);
            AVDictionary *tmp_dict = NULL;
            READ_MB(mb5_release_get_date, tmp_rel, tmp_dict, "date");
            READ_MB(mb5_release_get_title, tmp_rel, tmp_dict, "album");
            READ_MB(mb5_release_get_id, tmp_rel, tmp_dict, "id");
            cyanrip_log(ctx, 0, "    %i (id %s): %s (%s)", i + 1,
                        dict_get(tmp_dict, "id") ? dict_get(tmp_dict, "id") : "unknown id",
                        dict_get(tmp_dict, "album") ? dict_get(tmp_dict, "album") : "unknown album",
                        dict_get(tmp_dict, "date") ? dict_get(tmp_dict, "date") : "unknown date");

            /* Get CD count for the release */
            Mb5MediumList medium_list = mb5_release_get_mediumlist(tmp_rel);
            int num_cds = mb5_medium_list_size(medium_list);
            if (num_cds > 1)
                cyanrip_log(ctx, 0, " (%i CDs)", num_cds);

            cyanrip_log(ctx, 0, "\n");
            av_dict_free(&tmp_dict);
        }
        cyanrip_log(ctx, 0, "\n");
        cyanrip_log(ctx, 0, "Please specify which release to use by adding the -R argument with an index or id.\n");
        ret = 1;
        goto end_meta;
    } else if ((release_idx < 0) && !release_str) { /* Both unspecified, but only one release, so w/e */
        release_idx = 0;
    } else if (release_idx >= 0) { /* Release index specified */
        if ((release_idx < 1) || (release_idx > num_releases)) {
            cyanrip_log(ctx, 0, "Invalid release index %i specified, only have %i releases!\n", release_idx, num_releases);
            ret = 1;
            goto end_meta;
        }
        release_idx -= 1;
    } else if (release_str) { /* Release ID specified */
        int chosen_id = -1;
        for (int i = 0; i < num_releases; i++) {
            Mb5Release tmp_rel = mb5_release_list_item(release_list, i);
            AVDictionary *tmp_dict = NULL;
            READ_MB(mb5_release_get_id, tmp_rel, tmp_dict, "id");
            if (dict_get(tmp_dict, "id") && strcmp(release_str, dict_get(tmp_dict, "id"))) {
                av_dict_free(&tmp_dict);
                chosen_id = i;
                break;
            }
            av_dict_free(&tmp_dict);
        }
        if (chosen_id < 0) {
            cyanrip_log(ctx, 0, "Release id %s not found!\n", release_str);
            ret = 1;
            goto end_meta;
        }

        release_idx = chosen_id;
    }

    Mb5Release release = mb5_release_list_item(release_list, release_idx);

    READ_MB(mb5_release_get_date, release, ctx->meta, "date");
    READ_MB(mb5_release_get_title, release, ctx->meta, "album");
    Mb5ArtistCredit artistcredit = mb5_release_get_artistcredit(release);
    if (artistcredit)
        mb_credit(artistcredit, ctx->meta, "album_artist");

    cyanrip_log(ctx, 0, "Found MusicBrainz release: %s - %s\n",
                dict_get(ctx->meta, "album"), dict_get(ctx->meta, "album_artist"));

    /* Read track metadata */
    mb_tracks(ctx, release, discid, discnumber);

end_meta:
    mb5_metadata_delete(metadata); /* This frees _all_ metadata */

end:
    mb5_query_delete(query);
    return ret;
}

int crip_fill_metadata(cyanrip_ctx *ctx, int manual_metadata_specified,
                       int release_idx, char *release_str, int discnumber)
{
    /* Get musicbrainz tags */
    if (!ctx->settings.disable_mb)
        return mb_metadata(ctx, manual_metadata_specified, release_idx, release_str, discnumber);

    return 0;
}
