/*
 * Copyright © 2024, Lynne
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* genopt:
 * A small option parsing library in a single header, written in standard C11
 * License is a zero-clause MIT, feel free to use this wherever with no credit */

/* General usage:
 *
 * // optional, maximum list length that a single option may have (default: 64)
 * #define GEN_OPT_MAX_ARR 16
 *
 * // optional, logging callback (otherwise uses standard printf)
 * #define GEN_OPT_LOG avt_log
 *
 * // optional, second value given to the logging callback when printing
 * #define GEN_OPT_LOG_INFO AVT_LOG_INFO
 *
 * // optional, second value given to the logging callback on error
 * #define GEN_OPT_LOG_ERROR AVT_LOG_ERROR
 *
 * // optional, a rational value type containing `num` and `den`-named ints.
 * #define GEN_OPT_RATIONAL AVTRational
 *
 * // optional, defines a name and version to be used with --help and --version
 * #define GEN_OPT_HELPSTRING "project name " "0.1"
 *
 * #include "genopt.h"
 *
 * int main(int argc, char **argv)
 * {
 *     // Creates an option list with at most 16 entries
 *     GEN_OPT_INIT(opts_list, 16);
 *     // Adds an optional boolean option called unround, with a -u flag
 *     GEN_OPT_ONE(opts_list, bool  , unround, "u", 0, 0, 0, 0, "Boolean option called unround");
 *     // Adds a mandatory string option called output
 *     GEN_OPT_ONE(opts_list, char *, output,  "o", 1, 1, 0, 0, "Destination file");
 *     // Adds a mandatory string option array called input, with at most 8
 *     // comma-separated values. Options with a separator take their whole
 *     // list as a single argument (-i one,two), and respecifying the option
 *     // replaces the previous list. Options with no separator (0) take one
 *     // value per flag, and respecifying the option appends to the list.
 *     GEN_OPT_ARR(opts_list, char *, input,   "i", ',', 1, 8, 0, 0, "Input");
 *
 *     if (GEN_OPT_PARSE(NULL, opts_list, argc, argv) < 0)
 *         return EINVAL;
 *
 *     printf("Output was: %s\n", output);
 *     printf("Unround was: %i\n", unround);
 *
 *     and so on
 */

#ifndef GENOPT_HEADER
#define GENOPT_HEADER

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>

#ifndef GEN_OPT_MAX_ARR
#define GEN_OPT_MAX_ARR 64
#endif

#ifndef GEN_OPT_LOG
static inline void genopt_log(void *ctx, int error, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
#define GEN_OPT_LOG genopt_log
#define GEN_OPT_LOG_INFO 0
#define GEN_OPT_LOG_ERROR 1
#endif

#ifndef GEN_OPT_HELPSTRING
#define GEN_OPT_HELPSTRING "Program help"
#endif

#ifndef GEN_OPT_RATIONAL
typedef struct GenOptRational {
    int num;
    int den;
} GenOptRational;
#define GEN_OPT_RATIONAL GenOptRational
#endif

enum GenOptType {
    GEN_OPT_TYPE_NONE         = (0 << 0),
    GEN_OPT_TYPE_FLOAT        = (1 << 1),
    GEN_OPT_TYPE_DOUBLE       = (1 << 2),
    GEN_OPT_TYPE_I32          = (1 << 3),
    GEN_OPT_TYPE_U32          = (1 << 4),
    GEN_OPT_TYPE_U16          = (1 << 5),
    GEN_OPT_TYPE_I16          = (1 << 6),
    GEN_OPT_TYPE_I64          = (1 << 7),
    GEN_OPT_TYPE_U64          = (1 << 8),
    GEN_OPT_TYPE_BOOL         = (1 << 9),
    GEN_OPT_TYPE_NUM          = (GEN_OPT_TYPE_BOOL - 1) & (~1),

    GEN_OPT_TYPE_STRING       = (1 << 10),
    GEN_OPT_TYPE_RATIONAL     = (1 << 11),

    GEN_OPT_TYPE_SECTION      = (1 << 30),

    GEN_OPT_TYPE_UNKNOWN      = (1 << 29),
};

typedef struct GenOpt {
    const char *name;
    const char *flag;
    const char *flagname;

    enum GenOptType type;
    int min_vals;
    int max_vals;
    char sep;
    const char *help;
    void *val[GEN_OPT_MAX_ARR];

    double range_low_f;
    int64_t range_low_i;
    uint64_t range_low_u;
    double range_high_f;
    int64_t range_high_i;
    uint64_t range_high_u;

    /* Set at parse-time */
    bool present;
    int nb_vals;
} GenOpt;

#define GEN_OPT_SET(optlist, val, valname, valflag,                            \
                    minvals, maxvals, rangelow, rangehigh, helpstr)            \
    do {                                                                       \
        optlist[opts_list_nb] = (GenOpt) {                                     \
            .name = #valname,                                                  \
            .flag = "-"valflag,                                                \
            .flagname = "--"#valname,                                          \
            .type = _Generic((val),                                            \
                bool:                 GEN_OPT_TYPE_BOOL,                       \
                bool *:               GEN_OPT_TYPE_BOOL,                       \
                float:                GEN_OPT_TYPE_FLOAT,                      \
                float *:              GEN_OPT_TYPE_FLOAT,                      \
                double:               GEN_OPT_TYPE_DOUBLE,                     \
                double *:             GEN_OPT_TYPE_DOUBLE,                     \
                int32_t:              GEN_OPT_TYPE_I32,                        \
                int32_t *:            GEN_OPT_TYPE_I32,                        \
                uint32_t:             GEN_OPT_TYPE_U32,                        \
                uint32_t *:           GEN_OPT_TYPE_U32,                        \
                uint16_t:             GEN_OPT_TYPE_U16,                        \
                uint16_t *:           GEN_OPT_TYPE_U16,                        \
                int16_t:              GEN_OPT_TYPE_I16,                        \
                int16_t *:            GEN_OPT_TYPE_I16,                        \
                int64_t:              GEN_OPT_TYPE_I64,                        \
                int64_t *:            GEN_OPT_TYPE_I64,                        \
                uint64_t:             GEN_OPT_TYPE_U64,                        \
                uint64_t *:           GEN_OPT_TYPE_U64,                        \
                char *:               GEN_OPT_TYPE_STRING,                     \
                char **:              GEN_OPT_TYPE_STRING,                     \
                GEN_OPT_RATIONAL:     GEN_OPT_TYPE_RATIONAL,                   \
                GEN_OPT_RATIONAL *:   GEN_OPT_TYPE_RATIONAL,                   \
                default:              GEN_OPT_TYPE_UNKNOWN),                   \
            .help = helpstr,                                                   \
            .min_vals = minvals,                                               \
            .max_vals = maxvals,                                               \
        };                                                                     \
        switch (optlist[opts_list_nb].type) {                                  \
        case GEN_OPT_TYPE_FLOAT:                                               \
        case GEN_OPT_TYPE_DOUBLE:                                              \
        case GEN_OPT_TYPE_RATIONAL:                                            \
            optlist[opts_list_nb].range_low_f  = rangelow;                     \
            optlist[opts_list_nb].range_high_f = rangehigh;                    \
            break;                                                             \
        case GEN_OPT_TYPE_I16:                                                 \
        case GEN_OPT_TYPE_I32:                                                 \
        case GEN_OPT_TYPE_I64:                                                 \
            optlist[opts_list_nb].range_low_i  = rangelow;                     \
            optlist[opts_list_nb].range_high_i = rangehigh;                    \
            break;                                                             \
        case GEN_OPT_TYPE_U16:                                                 \
        case GEN_OPT_TYPE_U32:                                                 \
        case GEN_OPT_TYPE_U64:                                                 \
            optlist[opts_list_nb].range_low_u  = rangelow;                     \
            optlist[opts_list_nb].range_high_u = rangehigh;                    \
            break;                                                             \
        default:                                                               \
            break;                                                             \
        };                                                                     \
    } while (0)

#define GEN_OPT_ONE(optlist, valtype, valname, valflag,                        \
                    has_arg, req_val, def, rangelow, rangehigh, helpstr)       \
    valtype valname = def;                                                     \
    do {                                                                       \
        GEN_OPT_SET(optlist, valname, valname, valflag,                        \
                    ((has_arg) && (req_val)),                                  \
                    has_arg, rangelow, rangehigh, helpstr);                    \
        optlist[opts_list_nb].val[0] = _Generic((valname),                     \
                                                char *: (&(valname)),          \
                                                default: (&(valname)));        \
        opts_list_nb++;                                                        \
    } while (0)

#define GEN_OPT_ARR(optlist, valtype, valname, valflag, sepchar,               \
                    minvals, maxvals, rangelow, rangehigh, helpstr)            \
    valtype valname[maxvals];                                                  \
    memset(valname, 0, sizeof(valname));                                       \
    do {                                                                       \
        GEN_OPT_SET(optlist, valname, valname, valflag,                        \
                    minvals, maxvals, rangelow, rangehigh, helpstr);           \
        optlist[opts_list_nb].sep = sepchar;                                   \
                                                                               \
        for (int genopt_i = 0; genopt_i < maxvals; genopt_i++) {               \
            optlist[opts_list_nb].val[genopt_i] =                              \
                _Generic((valname[genopt_i]),                                  \
                         char *: (&(valname[genopt_i])),                       \
                         default: (&(valname[genopt_i])));                     \
        }                                                                      \
        opts_list_nb++;                                                        \
    } while (0)

#define GEN_OPT_SEC(optlist, section_name)   \
    do {                                     \
        optlist[opts_list_nb++] = (GenOpt) { \
            .name = section_name,            \
            .type = GEN_OPT_TYPE_SECTION,    \
        };                                   \
    } while (0)

/* Set option in an array/struct, rather than creating one */
#define GEN_OPT_VAR(optlist, struc, valname, valflag,                          \
                    has_arg, req_val, rangelow, rangehigh, helpstr)            \
    memset(&(struc.valname), 0, sizeof(struc.valname));                        \
    do {                                                                       \
        GEN_OPT_SET(optlist, struc.valname, valname, valflag,                  \
                    ((has_arg) && (req_val)), has_arg,                         \
                    rangelow, rangehigh, helpstr);                             \
        optlist[opts_list_nb].val[0] = &(struc.valname);                       \
        opts_list_nb++;                                                        \
    } while (0)

#define GEN_OPT_PARSE_FLT(opt, idx, data, type, fn)                            \
    do {                                                                       \
        type lval = fn(data, &endp);                                           \
        if (endp == data || *endp != '\0') {                                   \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                            \
                        "Error parsing \"%s\" as a " #type " for "             \
                        "argument \"%s\"\n",                                   \
                        data, l->name);                                        \
            return -EINVAL;                                                    \
        }                                                                      \
        /* Written so that NaN is out of range too */                          \
        if (!(lval >= l->range_low_f && lval <= l->range_high_f)) {            \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                            \
                        "Error parsing %s for argument \"%s\": "               \
                        "not in [%f:%f] range!\n",                             \
                        data, l->name, l->range_low_f, l->range_high_f);       \
            return -EINVAL;                                                    \
        }                                                                      \
        *((type *)opt->val[idx]) = lval;                                       \
    } while (0)

#define GEN_OPT_PARSE_INT(opt, idx, data, type, fn, base)                      \
    do {                                                                       \
        errno = 0;                                                             \
        int64_t lval = fn(data, &endp, base);                                  \
        if (endp == data || *endp != '\0') {                                   \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                            \
                        "Error parsing \"%s\" as a " #type " for "             \
                        "argument \"%s\"\n",                                   \
                        data, l->name);                                        \
            return -EINVAL;                                                    \
        }                                                                      \
        if (errno == ERANGE ||                                                 \
            lval > l->range_high_i || lval < l->range_low_i) {                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                            \
                        "Error parsing %s for argument \"%s\": "               \
                        "not in [%" PRIi64 ":%" PRIi64 "] range!\n",           \
                        data, l->name, l->range_low_i, l->range_high_i);       \
            return -EINVAL;                                                    \
        }                                                                      \
        *((type *)opt->val[idx]) = lval;                                       \
    } while (0)

#define GEN_OPT_PARSE_UINT(opt, idx, data, type, fn, base)                     \
    do {                                                                       \
        errno = 0;                                                             \
        uint64_t lval = fn(data, &endp, base);                                 \
        if (endp == data || *endp != '\0') {                                   \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                            \
                        "Error parsing \"%s\" as a " #type " for "             \
                        "argument \"%s\"\n",                                   \
                        data, l->name);                                        \
            return -EINVAL;                                                    \
        }                                                                      \
        /* strtoull() takes negative numbers, wrapping them around */          \
        if (errno == ERANGE || data[strspn(data, " \t")] == '-' ||             \
            lval > l->range_high_u || lval < l->range_low_u) {                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                            \
                        "Error parsing %s for argument \"%s\": "               \
                        "not in [%" PRIu64 ":%" PRIu64 "] range!\n",           \
                        data, l->name, l->range_low_u, l->range_high_u);       \
            return -EINVAL;                                                    \
        }                                                                      \
        *((type *)opt->val[idx]) = lval;                                       \
    } while (0)

#define GEN_OPT_PARSE_VAL(opt, idx, data)                                      \
    do {                                                                       \
        char *endp;                                                            \
        switch (opt->type) {                                                   \
        case GEN_OPT_TYPE_FLOAT:                                               \
            GEN_OPT_PARSE_FLT(opt, idx, data, float, strtof);                  \
            break;                                                             \
        case GEN_OPT_TYPE_DOUBLE:                                              \
            GEN_OPT_PARSE_FLT(opt, idx, data, double, strtod);                 \
            break;                                                             \
        case GEN_OPT_TYPE_I16:                                                 \
            GEN_OPT_PARSE_INT(opt, idx, data, int16_t, strtoll, 10);            \
            break;                                                             \
        case GEN_OPT_TYPE_I32:                                                 \
            GEN_OPT_PARSE_INT(opt, idx, data, int32_t, strtoll, 10);            \
            break;                                                             \
        case GEN_OPT_TYPE_I64:                                                 \
            GEN_OPT_PARSE_INT(opt, idx, data, int64_t, strtoll, 10);            \
            break;                                                             \
        case GEN_OPT_TYPE_U16:                                                 \
            GEN_OPT_PARSE_UINT(opt, idx, data, uint16_t, strtoull, 10);        \
            break;                                                             \
        case GEN_OPT_TYPE_U32:                                                 \
            GEN_OPT_PARSE_UINT(opt, idx, data, uint32_t, strtoull, 10);        \
            break;                                                             \
        case GEN_OPT_TYPE_U64:                                                 \
            GEN_OPT_PARSE_UINT(opt, idx, data, uint64_t, strtoull, 10);        \
            break;                                                             \
        case GEN_OPT_TYPE_RATIONAL: {                                          \
            char *end1, *end2;                                                 \
            int32_t arg1i, arg2i;                                              \
            char *arg1 = data;                                                 \
            char *arg2 = strchr(data, '/');                                    \
            GEN_OPT_RATIONAL *out;                                             \
            if (!arg2) {                                                       \
                GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                        \
                            "Error parsing value for argument \"%s\"\n",       \
                            l->name);                                          \
                return -EINVAL;                                                \
            }                                                                  \
            *arg2++ = '\0';                                                    \
            arg1i = strtol(arg1, &end1, 10);                                   \
            arg2i = strtol(arg2, &end2, 10);                                   \
            if (end1 == arg1 || *end1 != '\0' ||                               \
                end2 == arg2 || *end2 != '\0') {                               \
                GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                        \
                            "Error parsing value for argument \"%s\"\n",       \
                            l->name);                                          \
                return -EINVAL;                                                \
            }                                                                  \
            out = (GEN_OPT_RATIONAL *)opt->val[idx];                           \
            out->num = arg1i;                                                  \
            out->den = arg2i;                                                  \
            if ((out->num/(double)out->den) > l->range_high_f ||               \
                (out->num/(double)out->den) < l->range_low_f) {                \
                GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,                        \
                            "Error parsing %f for argument \"%s\": "           \
                            "range [%f:%f]!\n",                                \
                            (out->num/(double)out->den), l->name,              \
                            l->range_low_f, l->range_high_f);                  \
                return -EINVAL;                                                \
            }                                                                  \
            break;                                                             \
        }                                                                      \
        case GEN_OPT_TYPE_STRING:                                              \
            *((char **)opt->val[idx]) = data;                                  \
        break;                                                                 \
        default:                                                               \
            break;                                                             \
        }                                                                      \
    } while (0)

#define GEN_OPT_PRINT_DEFAULT_VAL(opt, idx)                                    \
    do {                                                                       \
        if (opt.max_vals > 1)                                                  \
            break;                                                             \
        switch (opt.type) {                                                    \
        case GEN_OPT_TYPE_FLOAT:                                               \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %f)",                                      \
                        *((float *)opt.val[idx]));                             \
            break;                                                             \
        case GEN_OPT_TYPE_DOUBLE:                                              \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %f)",                                      \
                        *((double *)opt.val[idx]));                            \
            break;                                                             \
        case GEN_OPT_TYPE_I16:                                                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %" PRIi16 ")",                             \
                        *((int16_t *)opt.val[idx]));                           \
            break;                                                             \
        case GEN_OPT_TYPE_I32:                                                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %" PRIi32 ")",                             \
                        *((int32_t *)opt.val[idx]));                           \
            break;                                                             \
        case GEN_OPT_TYPE_I64:                                                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %" PRIi64 ")",                             \
                        *((int64_t *)opt.val[idx]));                           \
            break;                                                             \
        case GEN_OPT_TYPE_U16:                                                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %" PRIu16 ")",                             \
                        *((uint16_t *)opt.val[idx]));                          \
            break;                                                             \
        case GEN_OPT_TYPE_U32:                                                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %" PRIu32 ")",                             \
                        *((uint32_t *)opt.val[idx]));                          \
            break;                                                             \
        case GEN_OPT_TYPE_U64:                                                 \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %" PRIu64 ")",                             \
                        *((uint64_t *)opt.val[idx]));                          \
            break;                                                             \
        case GEN_OPT_TYPE_BOOL:                                                \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %s)",                                      \
                        *((bool *)opt.val[idx]) ? "true" : "false");           \
            break;                                                             \
        case GEN_OPT_TYPE_RATIONAL: {                                          \
            GEN_OPT_RATIONAL *r = (GEN_OPT_RATIONAL *)opt.val[idx];            \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %i/%i)", r->num, r->den);                  \
            break;                                                             \
        }                                                                      \
        case GEN_OPT_TYPE_STRING:                                              \
            if (!(*(char **)opt.val[idx]))                                     \
                break;                                                         \
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO,                             \
                        " (default: %s)",                                      \
                        (*(char **)opt.val[idx]));                             \
            break;                                                             \
        default:                                                               \
            break;                                                             \
        }                                                                      \
    } while (0)

#define GEN_OPT_PHDR log_ctx, GEN_OPT_LOG_INFO, "    %s (%s):%*s%s"

/* Match argv flag against a registered flagname, translating '_' in the
 * registered name to '-'. So a variable named foo_bar registers as --foo-bar,
 * and only --foo-bar matches (--foo_bar does not). */
static inline int genopt_flagname_eq(const char *argv_str, const char *flagname)
{
    size_t i = 0;
    while (flagname[i]) {
        char c = flagname[i] == '_' ? '-' : flagname[i];
        if (argv_str[i] != c)
            return 0;
        i++;
    }
    return argv_str[i] == '\0';
}

/* Copy a registered flagname into buf, translating '_' to '-' for display. */
static inline const char *genopt_flagname_fmt(char *buf, size_t buflen,
                                              const char *s)
{
    size_t n = 0;
    while (s[n] && n + 1 < buflen) {
        buf[n] = s[n] == '_' ? '-' : s[n];
        n++;
    }
    buf[n] = '\0';
    return buf;
}

static inline int gen_opt_parse_fn(void *log_ctx, GenOpt *opts_list,
                                   int opts_list_nb, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        GenOpt *l = NULL;

        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO, "%s\n", GEN_OPT_HELPSTRING);
            return -EAGAIN;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            int pad_to = strlen("--version");
            for (int j = 0; j < opts_list_nb; j++) {
                if (opts_list[j].type == GEN_OPT_TYPE_SECTION)
                    continue;
                int opt_nl = strlen(opts_list[j].flagname);
                pad_to = opt_nl > pad_to ? opt_nl : pad_to;
            }
            pad_to += 1;

            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO, "%s\n", GEN_OPT_HELPSTRING);
            GEN_OPT_LOG(GEN_OPT_PHDR,
                        "--help", "-h", pad_to - (int)strlen("--help"), " ",
                        "Print this text");
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO, "\n");
            GEN_OPT_LOG(GEN_OPT_PHDR,
                        "--version", "-v", pad_to - (int)strlen("--version"), " ",
                        "Print the version number");
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO, "\n");
            for (int j = 0; j < opts_list_nb; j++) {
                if (opts_list[j].type == GEN_OPT_TYPE_SECTION) {
                    GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO, "\n%s:\n",
                                opts_list[j].name);
                    continue;
                }
                char fnbuf[64];
                GEN_OPT_LOG(GEN_OPT_PHDR,
                            genopt_flagname_fmt(fnbuf, sizeof(fnbuf),
                                                opts_list[j].flagname),
                            opts_list[j].flag,
                            pad_to - (int)strlen(opts_list[j].flagname), " ",
                            opts_list[j].help);
                GEN_OPT_PRINT_DEFAULT_VAL(opts_list[j], 0);
                GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_INFO, "\n");
            }
            return -EAGAIN;
        }

        for (int j = 0; j < opts_list_nb; j++) {
            if ((opts_list[j].flag && !strcmp(argv[i], opts_list[j].flag)) ||
                (opts_list[j].flagname &&
                 genopt_flagname_eq(argv[i], opts_list[j].flagname))) {
                l = &opts_list[j];
                break;
            }
        }

        if (!l) {
            GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,
                        "Unable to parse command line argument: %s\n",
                        argv[i]);
            return -EINVAL;
        } else if (l->max_vals == 0) {
            if (l->type != GEN_OPT_TYPE_BOOL) {
                GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,
                            "Programming error, incorrect type for: %s\n",
                            l->name);
                return -EINVAL;
            }
            bool *valptr = (bool *)l->val[0];
            *valptr = 1;
            l->present = 1;
        } else if (l->max_vals >= 1) {
            char fnbuf[64];
            if ((i + 1) >= argc) {
                GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,
                            "Missing value for argument \"%s\"\n",
                            genopt_flagname_fmt(fnbuf, sizeof(fnbuf),
                                                l->flagname));
                return -EINVAL;
            }

            /* Separator-split lists and single-value options replace any
             * previous occurrence; separator-less arrays accumulate */
            if ((l->sep || l->max_vals == 1) && l->nb_vals) {
                if (l->type == GEN_OPT_TYPE_STRING)
                    for (int j = 0; j < l->nb_vals; j++)
                        *((char **)l->val[j]) = NULL;
                l->nb_vals = 0;
            }

            char *data = argv[++i];
            while (data) {
                char *next = NULL;
                if (l->sep && ((next = strchr(data, l->sep)) != NULL))
                    *next++ = '\0';

                if (l->nb_vals >= l->max_vals) {
                    GEN_OPT_LOG(log_ctx, GEN_OPT_LOG_ERROR,
                                "Too many values for argument \"%s\" "
                                "(at most %i)\n",
                                genopt_flagname_fmt(fnbuf, sizeof(fnbuf),
                                                    l->flagname),
                                l->max_vals);
                    return -EINVAL;
                }

                GEN_OPT_PARSE_VAL(l, l->nb_vals, data);
                l->nb_vals++;
                data = next;
            }

            l->present = 1;
        }
    }

    return 0;
}

#define GEN_OPT_INIT(optlist, max) \
    GenOpt optlist[max];           \
    int optlist##_nb;              \
    do {                           \
        optlist##_nb = 0;          \
    } while (0)

#define GEN_OPT_PARSE(log_ctx, optlist, argc, argv)                   \
    gen_opt_parse_fn(log_ctx, optlist, optlist##_nb, argc, argv)      \

#endif
