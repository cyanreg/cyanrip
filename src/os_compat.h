/*
 * various OS-feature replacement utilities
 * copyright (c) 2000, 2001, 2002 Fabrice Bellard
 *
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

#pragma once

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <wchar.h>
#include <libavutil/mem.h>

static inline int utf8towchar(const char *filename_utf8, wchar_t **filename_w)
{
    int num_chars = MultiByteToWideChar(CP_UTF8, 0, filename_utf8, -1, NULL, 0);
    if (num_chars <= 0)
        abort();
    *filename_w = (wchar_t *)av_mallocz_array(num_chars, sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, filename_utf8, -1, *filename_w, num_chars);
    return 0;
}

static inline int win32_mkdir(const char *filename_utf8)
{
    wchar_t *filename_w;
    int ret;
    if (utf8towchar(filename_utf8, &filename_w))
        return -1;
    ret = _wmkdir(filename_w);
    av_free(filename_w);
    return ret;
}

static inline int win32_stat(const char *filename_utf8, struct stat* statbuf)
{
    wchar_t *filename_w;
    int ret;
    if (utf8towchar(filename_utf8, &filename_w))
        return -1;
    ret = _wstat64(filename_w, (struct _stat64 *)statbuf);
    av_free(filename_w);
    return ret;
}

#define mkdir(path, mode) win32_mkdir(path)
#define stat(path, statbuf) win32_stat(path, statbuf)
#endif

#if defined(__MACH__)
#define HAS_COLUMN 0
#endif

#if defined(HAVE_WMAIN)
#define HAS_CH_LESS 0
#define HAS_CH_MORE 0
#define HAS_CH_COLUMN 0
#define HAS_CH_OR 0
#define HAS_CH_Q 0
#define HAS_CH_ANY 0
#define HAS_CH_FWDSLASH 0
#define HAS_CH_BWDSLASH 0
#define HAS_CH_QUOTES 0
#endif

#ifndef HAS_CH_LESS
#define HAS_CH_LESS 1
#endif
#ifndef HAS_CH_MORE
#define HAS_CH_MORE 1
#endif
#ifndef HAS_CH_COLUMN
#define HAS_CH_COLUMN 1
#endif
#ifndef HAS_CH_OR
#define HAS_CH_OR 1
#endif
#ifndef HAS_CH_Q
#define HAS_CH_Q 1
#endif
#ifndef HAS_CH_ANY
#define HAS_CH_ANY 1
#endif
#ifndef HAS_CH_FWDSLASH
#define HAS_CH_FWDSLASH 0 // default should be 0 here
#endif
#ifndef HAS_CH_BWDSLASH
#define HAS_CH_BWDSLASH 1
#endif
#ifndef HAS_CH_QUOTES
#define HAS_CH_QUOTES 1
#endif
