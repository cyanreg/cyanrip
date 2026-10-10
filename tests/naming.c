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

#include <stdio.h>
#include <stdarg.h>

#include "cyanrip_main.h"
#include "cyanrip_log.h"

static int fails = 0;

/* naming.c logs scheme errors, give it somewhere to log to */
void cyanrip_log(cyanrip_ctx *ctx, int verbose, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static cyanrip_ctx ctx;
static const cyanrip_out_fmt fmt =
    { .name = "flac", .folder_suffix = "FLAC", .ext = "flac" };

static void check_str(const char *what, char *got, const char *want)
{
    if (!got || strcmp(got, want)) {
        printf("FAIL: %s:\n    got:  %s\n    want: %s\n",
               what, got ? got : "(null)", want);
        fails++;
    }
    av_free(got);
}

static void check_track_path(const char *scheme, cyanrip_track *t,
                             const char *want)
{
    ctx.settings.track_name_scheme = (char *)scheme;
    check_str(scheme, crip_get_path(&ctx, CRIP_PATH_TRACK, 0, &fmt, t), want);
}

int main(void)
{
    /* crip_is_integer() */
    if (!crip_is_integer("123") || crip_is_integer("12a") ||
        crip_is_integer("") || crip_is_integer(NULL)) {
        printf("FAIL: crip_is_integer\n");
        fails++;
    }

    /* append_missing_keys() */
    check_str("both keys missing",
              append_missing_keys("some_title:some_artist:key=value",
                                  "title=", "artist="),
              "title=some_title:artist=some_artist:key=value");
    check_str("no keys missing",
              append_missing_keys("title=a:artist=b", "title=", "artist="),
              "title=a:artist=b");
    check_str("escaped separators left alone",
              append_missing_keys("One\\: Two:some_artist", "title=", "artist="),
              "title=One\\: Two:artist=some_artist");

    /* crip_get_path() */
    ctx.settings.sanitize_method = CRIP_SANITIZE_UNICODE;
    ctx.settings.folder_name_scheme = "folder";
    ctx.nb_tracks = 2;

    av_dict_set(&ctx.meta, "album", "Album", 0);
    av_dict_set(&ctx.meta, "date", "2020-01-01", 0);

    cyanrip_track *t = &ctx.tracks[0];
    av_dict_set(&t->meta, "title", "Title", 0);
    av_dict_set(&t->meta, "track", "1", 0);
    av_dict_set(&t->meta, "disc", "2", 0);
    av_dict_set(&t->meta, "totaldiscs", "3", 0);

    check_track_path("{track} - {title}", t, "folder/1 - Title.flac");

    /* Literal text is never substituted, braces or bust */
    check_track_path("title", t, "folder/title.flac");
    check_track_path("iffy", t, "folder/iffy.flac");

    /* {format} resolves to the format name */
    check_track_path("{track} [{format}]", t, "folder/1 [flac].flac");

    /* Conditionals */
    check_track_path("{if #totaldiscs# > #1#|disc|.}{track}", t,
                     "folder/2.1.flac");
    av_dict_set(&t->meta, "totaldiscs", "1", 0);
    check_track_path("{if #totaldiscs# > #1#|disc|.}{track}", t,
                     "folder/1.flac");

    /* Sanitization of unrepresentable characters */
    av_dict_set(&t->meta, "title", "A/B: C\"D\"", 0);
    check_track_path("{title}", t, "folder/A∕B∶ C“D”.flac");
    av_dict_set(&t->meta, "title", "Title", 0);

    /* Track number zero padding, driven by the track count */
    ctx.nb_tracks = 12;
    check_track_path("{track}", t, "folder/01.flac");
    ctx.nb_tracks = 2;

    /* Folder schemes: {format} is the folder suffix, {year} is derived */
    ctx.settings.folder_name_scheme = "{album} ({year}) [{format}]";
    check_track_path("{track}", t, "Album (2020) [FLAC]/1.flac");
    ctx.settings.folder_name_scheme = "folder";

    /* Track-only tags in a log scheme fall back to literal text */
    ctx.settings.log_name_scheme = "{track}";
    check_str("log scheme with {track}",
              crip_get_path(&ctx, CRIP_PATH_LOG, 0, &fmt, NULL),
              "folder/track.log");

    /* Whitespace at component edges and before the extension is trimmed:
     * conditional bodies with spaces easily produce it on one branch */
    av_dict_set(&ctx.meta, "disc", "2", 0);
    av_dict_set(&ctx.meta, "totaldiscs", "3", 0);
    ctx.settings.folder_name_scheme = "{if #totaldiscs# > #1# CD|disc|}";
    check_track_path("{track}", t, "CD2/1.flac");
    ctx.settings.folder_name_scheme = "folder";

    av_dict_set(&t->meta, "totaldiscs", "3", 0);
    check_track_path("Disc {if #totaldiscs# > #1#|disc| of |totaldiscs|}", t,
                     "folder/Disc 2 of 3.flac");
    av_dict_set(&t->meta, "totaldiscs", "1", 0);
    check_track_path("Disc {if #totaldiscs# > #1#|disc| of |totaldiscs|}", t,
                     "folder/Disc.flac");
    check_track_path("  {track}", t, "folder/1.flac");

    /* Tokens matching no key compare as written */
    check_track_path("{if #nokey# == #nokey#same}{track}", t, "folder/same1.flac");
    check_track_path("{if #nokey# != #nokey#differ}{track}", t, "folder/1.flac");

    /* Syntax errors are caught without any metadata, as main() checks the
     * schemes before anything is looked up. The truncated conditions used
     * to leak their values. */
    static const char *const bad_schemes[] = {
        "{track", "{if #track#}", "{if #track# ~ #1#x}",
        "{if #track# == }", "{if #track# == #1}",
    };
    for (int i = 0; i < FF_ARRAY_ELEMS(bad_schemes); i++) {
        if (!crip_check_scheme(&ctx, bad_schemes[i])) {
            printf("FAIL: scheme \"%s\" passed the check\n", bad_schemes[i]);
            fails++;
        }
    }

    static const char *const good_schemes[] = {
        "{album}{if #releasecomment# > #0# (|releasecomment|)} [{format}]",
        "{if #totaldiscs# > #1#|disc|.}{track} - {title}",
        "{album}{if #totaldiscs# > #1# CD|disc|}", "plain", "",
    };
    for (int i = 0; i < FF_ARRAY_ELEMS(good_schemes); i++) {
        if (crip_check_scheme(&ctx, good_schemes[i])) {
            printf("FAIL: scheme \"%s\" failed the check\n", good_schemes[i]);
            fails++;
        }
    }

    if (fails) {
        printf("%i check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
