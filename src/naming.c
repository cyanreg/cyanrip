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

#include <sys/stat.h>

#include <libavutil/avstring.h>
#include <libavutil/bprint.h>

#include "cyanrip_main.h"
#include "cyanrip_log.h"
#include "os_compat.h"

/* Key 1 and 2 must be set */
char *append_missing_keys(const char *src, const char *key1, const char *key2)
{
    /* Copy string with enough space to append extra */
    char *copy = av_mallocz(strlen(src) + strlen(key1) + strlen(key2) + 1);
    memcpy(copy, src, strlen(src));

    int add_key1_offset = -1;
    int add_key2_offset = -1;

    /* Look for keyless entries in the first two, minding "\:" and "\="
     * escapes, which the dictionary parser will consume later */
    int count = 0, has_key = 0, esc = 0;
    int entry_start = 0;
    for (int i = 0; count < 2; i++) {
        char c = src[i];
        if (esc) {
            esc = 0;
        } else if (c == '\\') {
            esc = 1;
        } else if (c == '=') {
            has_key = 1;
        } else if (c == ':' || c == '\0') {
            if (!has_key && i > entry_start) {
                if (count == 0)
                    add_key1_offset = entry_start;
                else
                    add_key2_offset = entry_start;
            }
            count++;
            entry_start = i + 1;
            has_key = 0;
        }
        if (!c)
            break;
    }

    /* Prepend key1 if missing */
    if (add_key1_offset >= 0) {
        memmove(&copy[add_key1_offset + strlen(key1)], &copy[add_key1_offset], strlen(copy) - add_key1_offset);
        memcpy(&copy[add_key1_offset], key1, strlen(key1));
        if (add_key2_offset >= 0)
            add_key2_offset += strlen(key1);
    }

    /* Prepend key2 if missing */
    if (add_key2_offset >= 0) {
        memmove(&copy[add_key2_offset + strlen(key2)], &copy[add_key2_offset], strlen(copy) - add_key2_offset);
        memcpy(&copy[add_key2_offset], key2, strlen(key2));
    }

    return copy;
}

int crip_is_integer(const char *src)
{
    if (!src || !src[0])
        return 0;
    for (int i = 0; i < strlen(src); i++)
        if (!av_isdigit(src[i]))
            return 0;
    return 1;
}

struct CRIPCharReplacement {
    const char from;
    const char to;
    const char to_u[5];
    int is_avail_locally;
} crip_char_replacement[] = {
    { '<', '_', "‹", HAS_CH_LESS },
    { '>', '_', "›", HAS_CH_MORE },
    { ':', '_', "∶", HAS_CH_COLUMN },
    { '|', '_', "│", HAS_CH_OR },
    { '?', '_', "？", HAS_CH_Q },
    { '*', '_', "∗", HAS_CH_ANY },
    { '/', '_', "∕", HAS_CH_FWDSLASH },
    { '\\', '_', "⧹", HAS_CH_BWDSLASH },
    { '"', '\'', "“", HAS_CH_QUOTES },
    { '"', '\'', "”", HAS_CH_QUOTES },
    { 0 },
};

static int crip_bprint_sanitize(cyanrip_ctx *ctx, AVBPrint *buf, const char *str,
                                int sanitize_fwdslash)
{
    int32_t cp, ret, quote_match = 0;
    const char *pos = str, *end = str + strlen(str);

    int os_sanitize = (ctx->settings.sanitize_method == CRIP_SANITIZE_OS_SIMPLE) ||
                      (ctx->settings.sanitize_method == CRIP_SANITIZE_OS_UNICODE);

    while (str < end) {
        ret = av_utf8_decode(&cp, (const uint8_t **)&str, end, AV_UTF8_FLAG_ACCEPT_ALL);
        if (ret < 0) {
            cyanrip_log(ctx, 0, "Error parsing string: %s!\n", av_err2str(ret));
            return ret;
        }

        struct CRIPCharReplacement *rep = NULL;
        for (int i = 0; crip_char_replacement[i].from; i++) {
            if (cp == crip_char_replacement[i].from) {
                int is_quote = crip_char_replacement[i].from == '"';
                rep = &crip_char_replacement[i + (is_quote && quote_match)];
                quote_match = (quote_match + 1) & 1;
                break;
            }
        }

        int skip = !rep;
        int skip_sanitation = rep && (os_sanitize && rep->is_avail_locally);
        int passthrough_slash = rep && !skip_sanitation && (rep->from == OS_DIR_CHAR && !sanitize_fwdslash);

        if (skip || skip_sanitation || passthrough_slash) {
            av_bprint_append_data(buf, pos, str - pos);
            pos = str;
            continue;
        } else if (ctx->settings.sanitize_method == CRIP_SANITIZE_SIMPLE ||
                   ctx->settings.sanitize_method == CRIP_SANITIZE_OS_SIMPLE) {
            av_bprint_chars(buf, rep->to, 1);
        } else if (ctx->settings.sanitize_method == CRIP_SANITIZE_UNICODE ||
                   ctx->settings.sanitize_method == CRIP_SANITIZE_OS_UNICODE) {
            av_bprint_append_data(buf, rep->to_u, strlen(rep->to_u));
        }

        pos = str;
    }

    return 0;
}

static char *get_dir_tag_val(cyanrip_ctx *ctx, AVDictionary *meta,
                             const char *ofmt, const char *key)
{
    char *val = NULL;
    if (!strcmp(key, "year")) {
        const char *date = dict_get(meta, "date");
        if (date) {
            char *save_year, *date_dup = av_strdup(date);
            val = av_strdup(av_strtok(date_dup, ":-", &save_year));
            av_free(date_dup);
        }
    } else if (!strcmp(key, "format")) {
        val = av_strdup(ofmt);
    } else if (!strcmp(key, "track")) {
        const char *track = dict_get(meta, "track");
        if (crip_is_integer(track)) {
            int pad = 0, digits = strlen(track);
            if (((digits + pad) < 2) && ctx->nb_tracks >  9) pad++;
            if (((digits + pad) < 3) && ctx->nb_tracks > 99) pad++;
            val = av_mallocz(pad + digits + 1);
            for (int i = 0; i < pad; i++)
                val[i] = '0';
            memcpy(&val[pad], track, digits);
        } else {
            val = av_strdup(track);
        }
    } else {
        val = av_strdup(dict_get(meta, key));
    }

    return val;
}

static int process_cond(cyanrip_ctx *ctx, AVBPrint *buf, AVDictionary *meta,
                        const char *ofmt, const char *scheme)
{
    char *scheme_copy = av_strdup(scheme);

    char *pos = scheme_copy;
    while (*pos) {
        /* Literal text outside of {} is emitted as-is, never substituted */
        if (*pos != '{') {
            char *next = strchr(pos, '{');
            char tmpc = next ? *next : 0;
            if (next)
                *next = '\0';
            crip_bprint_sanitize(ctx, buf, pos, 0);
            if (!next)
                break;
            *next = tmpc;
            pos = next;
            continue;
        }

        char *end = strchr(pos + 1, '}');
        if (!end) {
            cyanrip_log(ctx, 0, "Invalid scheme syntax, unterminated \"{\"!\n");
            goto fail;
        }
        *end = '\0';
        char *tok = pos + 1;
        pos = end + 1;

        if (strchr(tok, '{')) {
            cyanrip_log(ctx, 0, "Invalid scheme syntax, \"{\" inside \"{\", "
                        "tags in conditions are written as |tag|!\n");
            goto fail;
        }

        if (!strncmp(tok, "if", strlen("if")) &&
            (tok[strlen("if")] == ' ' || tok[strlen("if")] == '#')) {
            char *cond = av_strdup(tok);
            char *cond_save, *cond_tok = av_strtok(cond, "#", &cond_save);

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no \"#\"!\n");
                av_free(cond);
                goto fail;
            }

            int val1_origin_is_tag = 1;
            char *val1 = get_dir_tag_val(ctx, meta, ofmt, cond_tok);
            if (!val1) {
                val1 = av_strdup(cond_tok);
                val1_origin_is_tag = 0;
            }

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no terminating \"#\"!\n");
                av_free(cond);
                av_free(val1);
                goto fail;
            }

            int cond_is_eq = 0, cond_is_not_eq = 0, cond_is_more = 0, cond_is_less = 0;
            if (strstr(cond_tok, "==")) {
                cond_is_eq = 1;
            } else if (strstr(cond_tok, "!=")) {
                cond_is_not_eq = 1;
            } else if (strstr(cond_tok, ">")) {
                cond_is_more = 1;
            } else if (strstr(cond_tok, "<")) {
                cond_is_less = 1;
            } else {
                cyanrip_log(ctx, 0, "Invalid condition syntax!\n");
                av_free(cond);
                av_free(val1);
                goto fail;
            }

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no terminating \"#\"!\n");
                av_free(cond);
                av_free(val1);
                goto fail;
            }

            int val2_origin_is_tag = 1;
            char *val2 = get_dir_tag_val(ctx, meta, ofmt, cond_tok);
            if (!val2) {
                val2 = av_strdup(cond_tok);
                val2_origin_is_tag = 0;
            }

            cond_tok = av_strtok(NULL, "#", &cond_save);
            if (!cond_tok) {
                cyanrip_log(ctx, 0, "Invalid scheme syntax, no terminating \"#\"!\n");
                av_free(cond);
                av_free(val1);
                av_free(val2);
                goto fail;
            }

            int cond_true = 0;
            cond_true |= cond_is_eq && !strcmp(val1, val2);
            cond_true |= cond_is_not_eq && strcmp(val1, val2);

            if (cond_is_less || cond_is_more) {
                int val1_is_int = crip_is_integer(val1), val2_is_int = crip_is_integer(val2);
                if (!val1_is_int && (val1_is_int == val2_is_int)) { /* None are int */
                    cond_true = cond_is_less ? (strcmp(val1, val2) < 0) : (cond_is_more ? strcmp(val1, val2) > 0 : 0);
                } else if (val1_is_int && (val1_is_int == val2_is_int)) { /* Both are int */
                    int64_t val1_dec = strtol(val1, NULL, 10);
                    int64_t val2_dec = strtol(val2, NULL, 10);
                    cond_true |= cond_is_less && val1_dec < val2_dec;
                    cond_true |= cond_is_more && val1_dec > val2_dec;
                } else {
                    ptrdiff_t val1_dec = val1_is_int ? strtol(val1, NULL, 10) : (!val1_origin_is_tag ? 0 : (ptrdiff_t)val1);
                    ptrdiff_t val2_dec = val2_is_int ? strtol(val2, NULL, 10) : (!val2_origin_is_tag ? 0 : (ptrdiff_t)val2);
                    cond_true |= cond_is_less && val1_dec < val2_dec;
                    cond_true |= cond_is_more && val1_dec > val2_dec;
                }
            }

            if (cond_true) {
                char *true_save, *true_tok = av_strtok(cond_tok, "|", &true_save);
                while (true_tok) {
                    int origin_is_tag = 1;
                    char *true_val = get_dir_tag_val(ctx, meta, ofmt, true_tok);
                    if (!true_val) {
                        true_val = av_strdup(true_tok);
                        origin_is_tag = 0;
                    }

                    crip_bprint_sanitize(ctx, buf, true_val, origin_is_tag);
                    av_free(true_val);

                    true_tok = av_strtok(NULL, "|", &true_save);
                }
            }

            av_free(val2);
            av_free(val1);
            av_free(cond);
            continue;
        }

        int origin_is_tag = 1;
        char *val = get_dir_tag_val(ctx, meta, ofmt, tok);
        if (!val) {
            val = av_strdup(tok);
            origin_is_tag = 0;
        }

        crip_bprint_sanitize(ctx, buf, val, origin_is_tag);
        av_free(val);
    }

    av_free(scheme_copy);
    return 0;

fail:
    av_free(scheme_copy);
    return AVERROR(EINVAL);
}

int crip_check_scheme(cyanrip_ctx *ctx, const char *scheme)
{
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    int ret = process_cond(ctx, &buf, NULL, "", scheme);
    av_bprint_finalize(&buf, NULL);
    return ret;
}

/* Schemes with conditionals easily produce spaces at the edges of path
 * components or before the extension, which no one ever wants */
static void crip_trim_path_components(char *path)
{
    char *dst = path, *src = path;

    while (*src) {
        /* Leading whitespace */
        while (*src == ' ' || *src == '\t')
            src++;

        /* Copy the component, tracking the end of its last non-space run */
        char *end = dst;
        while (*src && *src != OS_DIR_CHAR) {
            *dst++ = *src++;
            if (dst[-1] != ' ' && dst[-1] != '\t')
                end = dst;
        }
        dst = end;

        if (*src == OS_DIR_CHAR) {
            *dst++ = OS_DIR_CHAR;
            src++;
        }
    }
    *dst = '\0';

    /* Whitespace before the extension */
    char *dot = strrchr(path, '.');
    if (dot && dot > path) {
        char *ws = dot;
        while (ws > path && (ws[-1] == ' ' || ws[-1] == '\t'))
            ws--;
        if (ws != dot)
            memmove(ws, dot, strlen(dot) + 1);
    }
}

char *crip_get_path(cyanrip_ctx *ctx, enum CRIPPathType type, int create_dirs,
                    const cyanrip_out_fmt *fmt, void *arg)
{
    char *ret = NULL;
    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);

    /* A drive letter's colon would get sanitized like any other */
    const char *folder = ctx->settings.folder_name_scheme;
    if (av_toupper(folder[0]) >= 'A' && av_toupper(folder[0]) <= 'Z' &&
        folder[1] == ':' && (folder[2] == '/' || folder[2] == '\\')) {
        av_bprint_append_data(&buf, folder, 2);
        folder += 2;
    }

    if (process_cond(ctx, &buf, ctx->meta, fmt->folder_suffix, folder))
        goto end;

    av_bprint_chars(&buf, OS_DIR_CHAR, 1);

    char *ext = NULL;
    if (type == CRIP_PATH_COVERART) {
        CRIPArt *art = arg;
        crip_bprint_sanitize(ctx, &buf, dict_get(art->meta, "title"), 0);
        ext = art->extension ? av_strdup(art->extension) : av_strdup("<extension>");
    } else if (type == CRIP_PATH_LOG) {
        if (process_cond(ctx, &buf, ctx->meta, fmt->name,
                         ctx->settings.log_name_scheme))
            goto end;
        ext = av_strdup("log");
    } else if (type == CRIP_PATH_CUE) {
        if (process_cond(ctx, &buf, ctx->meta, fmt->name,
                         ctx->settings.cue_name_scheme))
            goto end;
        ext = av_strdup("cue");
    } else {
        cyanrip_track *t = arg;
        if (process_cond(ctx, &buf, t->meta, fmt->name,
                         ctx->settings.track_name_scheme))
            goto end;
        ext = av_strdup(t->track_is_data ? "bin" : fmt->ext);
    }

    if (ext)
        av_bprintf(&buf, ".%s", ext);
    av_free(ext);

end:
    av_bprint_finalize(&buf, &ret);
    if (!ret)
        return NULL;

    crip_trim_path_components(ret);

    if (create_dirs) {
        /* Create every directory making up the path */
        for (char *p = strchr(ret + 1, OS_DIR_CHAR); p;
             p = strchr(p + 1, OS_DIR_CHAR)) {
            *p = '\0';
            cyanrip_stat_t st_req = { 0 };
            if (cyanrip_stat(ret, &st_req) == -1)
                mkdir(ret, 0700);
            *p = OS_DIR_CHAR;
        }
    }

    return ret;
}
