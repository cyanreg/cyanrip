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
#include <string.h>

/* Keep the parser's messages, rather than printing them */
static char last_msg[512];
static void test_log(void *ctx, int level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_msg, sizeof(last_msg), fmt, args);
    va_end(args);
}
#define GEN_OPT_LOG test_log
#define GEN_OPT_LOG_INFO 0
#define GEN_OPT_LOG_ERROR 1

#include "genopt.h"

static int fails = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL: %s:%i: %s\n", __func__, __LINE__, #cond);    \
            fails++;                                                   \
        }                                                              \
    } while (0)

/* The parser writes into its arguments, so give it copies */
static char argbuf[32][128];
static char *args[33];
static int set_args(const char *first, ...)
{
    va_list ap;
    int argc = 0;
    args[argc] = strcpy(argbuf[argc], "prog");
    argc++;

    va_start(ap, first);
    for (const char *a = first; a; a = va_arg(ap, const char *)) {
        args[argc] = strcpy(argbuf[argc], a);
        argc++;
    }
    va_end(ap);
    args[argc] = NULL;

    return argc;
}

/* The options under test, as cyanrip declares its own */
#define DECLARE_OPTS                                                                  \
    GEN_OPT_INIT(opts_list, 16);                                                      \
    GEN_OPT_ONE(opts_list, int32_t, offset, "s", 1, 1, 0, INT32_MIN, INT32_MAX, "");  \
    GEN_OPT_ONE(opts_list, int32_t, repeat_rips, "Z", 1, 1, 0, 0, INT32_MAX, "");     \
    GEN_OPT_ONE(opts_list, int64_t, big, "g", 1, 1, 0, INT64_MIN, INT64_MAX, "");     \
    GEN_OPT_ONE(opts_list, uint32_t, unsig, "u", 1, 1, 7, 0, UINT32_MAX, "");         \
    GEN_OPT_ONE(opts_list, float, bitrate, "b", 1, 1, 256.0f, 0.0f, 10000.0f, "");    \
    GEN_OPT_ONE(opts_list, char *, device, "d", 1, 1, NULL, 0, 0, "");                \
    GEN_OPT_ONE(opts_list, bool, no_accurip, "A", 0, 0, 0, 0, 0, "");                 \
    GEN_OPT_ONE(opts_list, bool, no_musicbrainz, "N", 0, 0, 0, 0, 0, "");             \
    GEN_OPT_ONE(opts_list, GEN_OPT_RATIONAL, rate, "q", 1, 1, ((GEN_OPT_RATIONAL){ 1, 1 }), \
                0, 1000, "");                                                         \
    GEN_OPT_ARR(opts_list, char *, outputs, "o", ',', 0, 4, 0, 0, "");                \
    GEN_OPT_ARR(opts_list, int32_t, tracks, "l", ',', 0, 8, 0, 198, "");              \
    GEN_OPT_ARR(opts_list, char *, track_meta, "t", 0, 0, 4, 0, 0, "")

static void test_values(void)
{
    DECLARE_OPTS;
    int argc = set_args("-s", "-6", "--repeat-rips", "3", "-b", "320.5", "-d",
                        "/dev/sr1", "-A", "-q", "30000/1001", NULL);
    CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == 0);
    CHECK(offset == -6);
    CHECK(repeat_rips == 3);
    CHECK(bitrate == 320.5f);
    CHECK(device && !strcmp(device, "/dev/sr1"));
    CHECK(no_accurip && !no_musicbrainz);
    CHECK(rate.num == 30000 && rate.den == 1001);

    /* Untouched options keep their defaults */
    CHECK(unsig == 7 && big == 0);
    CHECK(!opts_list[2].present && !opts_list[3].present);
}

static void test_arrays(void)
{
    /* A separator splits one argument, and respecifying replaces the list */
    {
        DECLARE_OPTS;
        int argc = set_args("-o", "flac,mp3", "-l", "1,2,3", NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == 0);
        CHECK(opts_list[9].nb_vals == 2);
        CHECK(!strcmp(outputs[0], "flac") && !strcmp(outputs[1], "mp3"));
        CHECK(opts_list[10].nb_vals == 3 && tracks[2] == 3);
    }
    {
        DECLARE_OPTS;
        int argc = set_args("-o", "flac,mp3,opus", "-o", "wav", NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == 0);
        CHECK(opts_list[9].nb_vals == 1);
        CHECK(!strcmp(outputs[0], "wav") && !outputs[1] && !outputs[2]);
    }

    /* No separator takes one value per flag, appending */
    {
        DECLARE_OPTS;
        int argc = set_args("-t", "1=title=a:artist=b", "-t", "2=title=c,d", NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == 0);
        CHECK(opts_list[11].nb_vals == 2);
        CHECK(!strcmp(track_meta[0], "1=title=a:artist=b"));
        CHECK(!strcmp(track_meta[1], "2=title=c,d"));
    }

    /* Too many values, either way */
    {
        DECLARE_OPTS;
        int argc = set_args("-o", "a,b,c,d,e", NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == -EINVAL);
    }
    {
        DECLARE_OPTS;
        int argc = set_args("-t", "1", "-t", "2", "-t", "3", "-t", "4", "-t", "5", NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == -EINVAL);
    }

    /* Array elements are range checked one by one */
    {
        DECLARE_OPTS;
        int argc = set_args("-l", "1,199", NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == -EINVAL);
    }
}

static void test_respecified(void)
{
    DECLARE_OPTS;
    int argc = set_args("-s", "6", "--offset", "12", "-d", "a", "-d", "b", NULL);
    CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == 0);
    CHECK(offset == 12);
    CHECK(!strcmp(device, "b"));
}

/* Each must fail to parse */
static void test_rejected(const char *what, ...)
{
    va_list ap;
    const char *a[8] = { NULL };
    int n = 0;
    va_start(ap, what);
    while (n < 7 && (a[n] = va_arg(ap, const char *)))
        n++;
    va_end(ap);

    DECLARE_OPTS;
    int argc = set_args(a[0], a[1], a[2], a[3], a[4], a[5], a[6], NULL);
    if (GEN_OPT_PARSE(NULL, opts_list, argc, args) != -EINVAL) {
        printf("FAIL: %s was accepted\n", what);
        fails++;
    }
}

static void test_errors(void)
{
    test_rejected("unknown flag", "-x", NULL);
    test_rejected("missing value", "-s", NULL);
    test_rejected("not a number", "-s", "abc", NULL);
    test_rejected("trailing garbage", "-s", "12a", NULL);
    test_rejected("empty number", "-s", "", NULL);
    test_rejected("out of range", "-Z", "-1", NULL);
    test_rejected("int32 overflow", "-s", "2147483648", NULL);
    test_rejected("int64 overflow", "-g", "99999999999999999999", NULL);
    test_rejected("int64 underflow", "-g", "-99999999999999999999", NULL);
    test_rejected("negative unsigned", "-u", "-1", NULL);
    test_rejected("unsigned overflow", "-u", "4294967296", NULL);
    test_rejected("float out of range", "-b", "10001", NULL);
    test_rejected("float NaN", "-b", "nan", NULL);
    test_rejected("float infinity", "-b", "inf", NULL);
    test_rejected("rational without slash", "-q", "30", NULL);
    test_rejected("rational garbage", "-q", "30/x", NULL);

    /* Accepted compromises against getopt: no combined short flags, and no
     * values attached to their flags */
    test_rejected("combined short flags", "-NA", NULL);
    test_rejected("attached short value", "-s100", NULL);
    test_rejected("attached long value", "--offset=100", NULL);

    /* Long names take dashes, not the underscores of the variable names */
    test_rejected("underscored long name", "--repeat_rips", "1", NULL);
}

static void test_limits(void)
{
    DECLARE_OPTS;
    int argc = set_args("-s", "-2147483648", "-g", "-9223372036854775808",
                        "-u", "4294967295", NULL);
    CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == 0);
    CHECK(offset == INT32_MIN);
    CHECK(big == INT64_MIN);
    CHECK(unsig == UINT32_MAX);
}

static void test_help(void)
{
    /* Values are taken as they are, even ones that look like flags */
    {
        DECLARE_OPTS;
        int argc = set_args("-d", "-h", "-o", "--version", NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == 0);
        CHECK(!strcmp(device, "-h") && !strcmp(outputs[0], "--version"));
    }

    const char *quit[] = { "-h", "--help", "-v", "--version" };
    for (int i = 0; i < 4; i++) {
        DECLARE_OPTS;
        int argc = set_args(quit[i], NULL);
        CHECK(GEN_OPT_PARSE(NULL, opts_list, argc, args) == -EAGAIN);
    }
}

int main(void)
{
    test_values();
    test_arrays();
    test_respecified();
    test_errors();
    test_limits();
    test_help();

    if (fails) {
        printf("%i check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
