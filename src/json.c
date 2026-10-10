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

#include <stdint.h>
#include <string.h>

#include <libavutil/common.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>

#include "json.h"

/* Replies are a few levels deep at most, anything past this is garbage */
#define JSON_MAX_DEPTH 32

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Past the closing quote of the string at p */
static const char *skip_str(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '"')
            return p + 1;
        if (*p == '\\')
            p++;
    }
    return NULL;
}

/* Past the value at p, NULL if it is malformed */
static const char *skip_val(const char *p, const char *end, int depth)
{
    p = skip_ws(p, end);
    if (p >= end)
        return NULL;

    if (*p == '"')
        return skip_str(p, end);

    if (*p == '{' || *p == '[') {
        const char close = *p == '{' ? '}' : ']';
        if (depth >= JSON_MAX_DEPTH)
            return NULL;

        p = skip_ws(p + 1, end);
        if (p < end && *p == close)
            return p + 1;

        while (1) {
            if (close == '}') {
                p = skip_ws(p, end);
                if (p >= end || *p != '"' || !(p = skip_str(p, end)))
                    return NULL;
                p = skip_ws(p, end);
                if (p >= end || *p != ':')
                    return NULL;
                p++;
            }

            if (!(p = skip_val(p, end, depth + 1)))
                return NULL;

            p = skip_ws(p, end);
            if (p >= end)
                return NULL;
            if (*p == close)
                return p + 1;
            if (*p != ',')
                return NULL;
            p++;
        }
    }

    /* Numbers, true, false and null */
    const char *start = p;
    while (p < end && (av_isdigit(*p) || (*p >= 'a' && *p <= 'z') ||
                       *p == '-' || *p == '+' || *p == '.' || *p == 'E'))
        p++;

    return p > start ? p : NULL;
}

const char *crip_json_get(const char *p, const char *end, const char *key)
{
    if (!p)
        return NULL;

    p = skip_ws(p, end);
    if (p >= end || *p != '{')
        return NULL;

    p = skip_ws(p + 1, end);
    if (p < end && *p == '}')
        return NULL;

    while (1) {
        p = skip_ws(p, end);

        char *name = crip_json_str(p, end);
        if (!name)
            return NULL;
        int match = !strcmp(name, key);
        av_free(name);

        p = skip_ws(skip_str(p, end), end);
        if (p >= end || *p != ':')
            return NULL;

        const char *val = skip_ws(p + 1, end);
        if (!(p = skip_val(val, end, 1)))
            return NULL;
        if (match)
            return val;

        p = skip_ws(p, end);
        if (p >= end || *p != ',')
            return NULL;
        p++;
    }
}

int crip_json_count(const char *p, const char *end)
{
    if (!p)
        return -1;

    p = skip_ws(p, end);
    if (p >= end || *p != '[')
        return -1;

    p = skip_ws(p + 1, end);
    if (p < end && *p == ']')
        return 0;

    for (int nb = 1; ; nb++) {
        if (!(p = skip_val(p, end, 1)))
            return -1;

        p = skip_ws(p, end);
        if (p >= end)
            return -1;
        if (*p == ']')
            return nb;
        if (*p != ',')
            return -1;
        p++;
    }
}

static int read_hex4(const char *p, const char *end, uint32_t *val)
{
    if (end - p < 4)
        return -1;

    *val = 0;
    for (int i = 0; i < 4; i++) {
        int c = av_tolower(p[i]);
        if (av_isdigit(c))
            *val = (*val << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f')
            *val = (*val << 4) | (c - 'a' + 10);
        else
            return -1;
    }

    return 0;
}

char *crip_json_str(const char *p, const char *end)
{
    if (!p)
        return NULL;

    p = skip_ws(p, end);
    if (p >= end || *p != '"')
        return NULL;

    const char *str_end = skip_str(p, end);
    if (!str_end)
        return NULL;
    str_end--; /* The closing quote */

    /* Unescaping never lengthens the text, quotes make room for the NUL */
    char *out = av_malloc(str_end - p + 1);
    if (!out)
        return NULL;

    char *o = out;
    for (p++; p < str_end; p++) {
        if (*p != '\\') {
            *o++ = *p;
            continue;
        }

        switch (*(++p)) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            uint32_t c, lo;
            if (read_hex4(p + 1, str_end, &c) < 0)
                goto fail;
            p += 4;

            /* Characters outside the BMP come as a UTF-16 surrogate pair */
            if (c >= 0xD800 && c < 0xDC00) {
                if (str_end - p < 7 || p[1] != '\\' || p[2] != 'u' ||
                    read_hex4(p + 3, str_end, &lo) < 0 ||
                    lo < 0xDC00 || lo >= 0xE000)
                    goto fail;
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            } else if (c >= 0xDC00 && c < 0xE000) {
                goto fail;
            }

            uint8_t tmp;
            PUT_UTF8(c, tmp, *o++ = tmp;)
            break;
        }
        default: /* Quote, backslash and slash */
            *o++ = *p;
            break;
        }
    }

    *o = '\0';
    return out;

fail:
    av_free(out);
    return NULL;
}
