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
#include <string.h>

#include <libavutil/mem.h>

#include "json.h"

static int fails = 0;

static void check_str(const char *what, char *got, const char *want)
{
    if ((!got != !want) || (got && strcmp(got, want))) {
        printf("FAIL: %s:\n    got:  %s\n    want: %s\n",
               what, got ? got : "(null)", want ? want : "(null)");
        fails++;
    }
    av_free(got);
}

static void check_int(const char *what, int got, int want)
{
    if (got != want) {
        printf("FAIL: %s: got %i, want %i\n", what, got, want);
        fails++;
    }
}

/* Member key of the top-level object of json, as a string */
static char *get_str(const char *json, const char *key)
{
    const char *end = json + strlen(json);
    return crip_json_str(crip_json_get(json, end, key), end);
}

static int count(const char *json)
{
    return crip_json_count(json, json + strlen(json));
}

int main(void)
{
    /* Replies as AcoustID sends them */
    const char *ok = "{\"status\": \"ok\", \"submissions\": ["
                     "{\"index\": \"0\", \"status\": \"pending\", \"id\": 123456789}, "
                     "{\"index\": \"1\", \"status\": \"pending\", \"id\": 123456790}]}";
    const char *err = "{\"status\": \"error\", \"error\": "
                      "{\"code\": 6, \"message\": \"invalid user API key\"}}";
    const char *ok_end = ok + strlen(ok);
    const char *err_end = err + strlen(err);

    check_str("status", get_str(ok, "status"), "ok");
    check_int("submissions", crip_json_count(crip_json_get(ok, ok_end, "submissions"),
                                             ok_end), 2);
    check_str("error message",
              crip_json_str(crip_json_get(crip_json_get(err, err_end, "error"),
                                          err_end, "message"), err_end),
              "invalid user API key");

    /* Members are found past values of every kind, nested ones included */
    check_str("member past others",
              get_str("{\"a\": [1, {\"b\": \"}\"}, [], -1.5e+3, true, null], "
                      "\"c\": {}, \"d\": \"found\"}", "d"),
              "found");
    check_str("missing member", get_str("{\"a\": \"b\"}", "status"), NULL);
    check_str("not an object", get_str("[\"status\"]", "status"), NULL);
    check_str("empty object", get_str("{ }", "status"), NULL);
    check_str("not a string", get_str("{\"status\": 1}", "status"), NULL);
    check_str("no whitespace", get_str("{\"x\":[],\"status\":\"ok\"}", "status"), "ok");

    /* Escapes, in values and in keys */
    check_str("escapes", get_str("{\"s\": \"q\\\"b\\\\s\\/n\\nt\\t\"}", "s"),
              "q\"b\\s/n\nt\t");
    check_str("escaped key", get_str("{\"st\\u0061tus\": \"ok\"}", "status"), "ok");
    check_str("BMP escape", get_str("{\"s\": \"\\u00e9\\u30ab\"}", "s"),
              "\xc3\xa9\xe3\x82\xab");
    check_str("surrogate pair", get_str("{\"s\": \"\\ud83c\\udfb5\"}", "s"),
              "\xf0\x9f\x8e\xb5");
    check_str("lone high surrogate", get_str("{\"s\": \"\\ud83c\"}", "s"), NULL);
    check_str("lone low surrogate", get_str("{\"s\": \"\\udfb5\"}", "s"), NULL);
    check_str("short escape", get_str("{\"s\": \"\\u12\"}", "s"), NULL);

    /* Malformed or cut short replies give nothing, rather than reading on */
    check_str("unterminated string", get_str("{\"status\": \"ok", "status"), NULL);
    check_str("unterminated object", get_str("{\"a\": [1, 2", "status"), NULL);
    check_str("missing colon", get_str("{\"a\" 1, \"status\": \"ok\"}", "status"), NULL);
    check_str("member after garbage", get_str("{\"a\": , \"status\": \"ok\"}", "status"), NULL);
    check_str("trailing backslash", get_str("{\"status\": \"ok\\", "status"), NULL);

    /* Cut off by the end pointer rather than a NUL */
    const char *cut = "{\"status\": \"ok\"}";
    check_str("value past the end",
              crip_json_str(crip_json_get(cut, cut + 12, "status"), cut + 12), NULL);

    /* Nesting past the limit is refused rather than recursed into */
    char deep[256] = "{\"a\": ";
    for (int i = 0; i < 64; i++)
        strcat(deep, "[");
    for (int i = 0; i < 64; i++)
        strcat(deep, "]");
    strcat(deep, ", \"status\": \"ok\"}");
    check_str("deep nesting", get_str(deep, "status"), NULL);

    check_int("array count", count("[1, [2, 3], {}]"), 3);
    check_int("empty array", count("[ ]"), 0);
    check_int("not an array", count("{}"), -1);
    check_int("unterminated array", count("[1, 2"), -1);
    check_int("NULL array", crip_json_count(NULL, NULL), -1);

    if (fails) {
        printf("%i check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
