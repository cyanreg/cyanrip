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

#include "../config.h"
#include "qrcode.h"

#ifdef HAVE_QRENCODE

#include <stdio.h>
#include <stdint.h>

#include <qrencode.h>
#include <libavutil/bprint.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define crip_isatty() _isatty(_fileno(stdout))
#else
#include <unistd.h>
#define crip_isatty() isatty(fileno(stdout))
#endif

/* Quiet zone around the symbol, in modules, as the spec demands */
#define QR_MARGIN 4

/* Each cell carries a 2x4 grid of modules, drawn with the octant glyphs from
 * Unicode 16. That keeps a module exactly as square as the half blocks did,
 * since both assume a 2:1 cell, but at a quarter of the area. */
#define QR_CELL_W 2
#define QR_CELL_H 4

/* Fixed palette cube entries, which terminal themes don't remap, unlike the
 * first 16. The image is a negative: set modules are light, clear ones dark. */
#define QR_COL_SET   "231"
#define QR_COL_CLEAR "16"

/* Codepoint per 2x4 module pattern, bit n being module (n%2, n/2) of the cell.
 * Six patterns have no glyph of their own and are left as 0; those are drawn
 * as their complement with the two colours swapped. */
static const uint32_t qr_octants[256] = {
    0x00020, 0x00000, 0x00000, 0x1FB82, 0x1CD00, 0x02598, 0x1CD01, 0x1CD02,
    0x1CD03, 0x1CD04, 0x0259D, 0x1CD05, 0x1CD06, 0x1CD07, 0x1CD08, 0x02580,
    0x1CD09, 0x1CD0A, 0x1CD0B, 0x1CD0C, 0x00000, 0x1CD0D, 0x1CD0E, 0x1CD0F,
    0x1CD10, 0x1CD11, 0x1CD12, 0x1CD13, 0x1CD14, 0x1CD15, 0x1CD16, 0x1CD17,
    0x1CD18, 0x1CD19, 0x1CD1A, 0x1CD1B, 0x1CD1C, 0x1CD1D, 0x1CD1E, 0x1CD1F,
    0x00000, 0x1CD20, 0x1CD21, 0x1CD22, 0x1CD23, 0x1CD24, 0x1CD25, 0x1CD26,
    0x1CD27, 0x1CD28, 0x1CD29, 0x1CD2A, 0x1CD2B, 0x1CD2C, 0x1CD2D, 0x1CD2E,
    0x1CD2F, 0x1CD30, 0x1CD31, 0x1CD32, 0x1CD33, 0x1CD34, 0x1CD35, 0x1FB85,
    0x00000, 0x1CD36, 0x1CD37, 0x1CD38, 0x1CD39, 0x1CD3A, 0x1CD3B, 0x1CD3C,
    0x1CD3D, 0x1CD3E, 0x1CD3F, 0x1CD40, 0x1CD41, 0x1CD42, 0x1CD43, 0x1CD44,
    0x02596, 0x1CD45, 0x1CD46, 0x1CD47, 0x1CD48, 0x0258C, 0x1CD49, 0x1CD4A,
    0x1CD4B, 0x1CD4C, 0x0259E, 0x1CD4D, 0x1CD4E, 0x1CD4F, 0x1CD50, 0x0259B,
    0x1CD51, 0x1CD52, 0x1CD53, 0x1CD54, 0x1CD55, 0x1CD56, 0x1CD57, 0x1CD58,
    0x1CD59, 0x1CD5A, 0x1CD5B, 0x1CD5C, 0x1CD5D, 0x1CD5E, 0x1CD5F, 0x1CD60,
    0x1CD61, 0x1CD62, 0x1CD63, 0x1CD64, 0x1CD65, 0x1CD66, 0x1CD67, 0x1CD68,
    0x1CD69, 0x1CD6A, 0x1CD6B, 0x1CD6C, 0x1CD6D, 0x1CD6E, 0x1CD6F, 0x1CD70,
    0x00000, 0x1CD71, 0x1CD72, 0x1CD73, 0x1CD74, 0x1CD75, 0x1CD76, 0x1CD77,
    0x1CD78, 0x1CD79, 0x1CD7A, 0x1CD7B, 0x1CD7C, 0x1CD7D, 0x1CD7E, 0x1CD7F,
    0x1CD80, 0x1CD81, 0x1CD82, 0x1CD83, 0x1CD84, 0x1CD85, 0x1CD86, 0x1CD87,
    0x1CD88, 0x1CD89, 0x1CD8A, 0x1CD8B, 0x1CD8C, 0x1CD8D, 0x1CD8E, 0x1CD8F,
    0x02597, 0x1CD90, 0x1CD91, 0x1CD92, 0x1CD93, 0x0259A, 0x1CD94, 0x1CD95,
    0x1CD96, 0x1CD97, 0x02590, 0x1CD98, 0x1CD99, 0x1CD9A, 0x1CD9B, 0x0259C,
    0x1CD9C, 0x1CD9D, 0x1CD9E, 0x1CD9F, 0x1CDA0, 0x1CDA1, 0x1CDA2, 0x1CDA3,
    0x1CDA4, 0x1CDA5, 0x1CDA6, 0x1CDA7, 0x1CDA8, 0x1CDA9, 0x1CDAA, 0x1CDAB,
    0x02582, 0x1CDAC, 0x1CDAD, 0x1CDAE, 0x1CDAF, 0x1CDB0, 0x1CDB1, 0x1CDB2,
    0x1CDB3, 0x1CDB4, 0x1CDB5, 0x1CDB6, 0x1CDB7, 0x1CDB8, 0x1CDB9, 0x1CDBA,
    0x1CDBB, 0x1CDBC, 0x1CDBD, 0x1CDBE, 0x1CDBF, 0x1CDC0, 0x1CDC1, 0x1CDC2,
    0x1CDC3, 0x1CDC4, 0x1CDC5, 0x1CDC6, 0x1CDC7, 0x1CDC8, 0x1CDC9, 0x1CDCA,
    0x1CDCB, 0x1CDCC, 0x1CDCD, 0x1CDCE, 0x1CDCF, 0x1CDD0, 0x1CDD1, 0x1CDD2,
    0x1CDD3, 0x1CDD4, 0x1CDD5, 0x1CDD6, 0x1CDD7, 0x1CDD8, 0x1CDD9, 0x1CDDA,
    0x02584, 0x1CDDB, 0x1CDDC, 0x1CDDD, 0x1CDDE, 0x02599, 0x1CDDF, 0x1CDE0,
    0x1CDE1, 0x1CDE2, 0x0259F, 0x1CDE3, 0x02586, 0x1CDE4, 0x1CDE5, 0x02588,
};

static int qr_module(const QRcode *qr, int x, int y)
{
    /* Anything outside the symbol is quiet zone, e.g. clear */
    if (x < 0 || y < 0 || x >= qr->width || y >= qr->width)
        return 0;
    return qr->data[y*qr->width + x] & 1;
}

static void qr_put_utf8(AVBPrint *buf, uint32_t cp)
{
    if (cp < 0x80) {
        av_bprint_chars(buf, cp, 1);
    } else if (cp < 0x800) {
        av_bprint_chars(buf, 0xC0 | (cp >> 6), 1);
        av_bprint_chars(buf, 0x80 | (cp & 0x3F), 1);
    } else if (cp < 0x10000) {
        av_bprint_chars(buf, 0xE0 | (cp >> 12), 1);
        av_bprint_chars(buf, 0x80 | ((cp >> 6) & 0x3F), 1);
        av_bprint_chars(buf, 0x80 | (cp & 0x3F), 1);
    } else {
        av_bprint_chars(buf, 0xF0 | (cp >> 18), 1);
        av_bprint_chars(buf, 0x80 | ((cp >> 12) & 0x3F), 1);
        av_bprint_chars(buf, 0x80 | ((cp >> 6) & 0x3F), 1);
        av_bprint_chars(buf, 0x80 | (cp & 0x3F), 1);
    }
}

#endif

void crip_print_qrcode(const char *url)
{
#ifdef HAVE_QRENCODE
    if (!url || !crip_isatty())
        return;

    QRcode *qr = QRcode_encodeString(url, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (!qr)
        return;

#ifdef _WIN32
    HANDLE con = GetStdHandle(STD_OUTPUT_HANDLE);
    UINT old_cp = GetConsoleOutputCP();
    DWORD old_mode;
    int mode_changed = GetConsoleMode(con, &old_mode) &&
                       SetConsoleMode(con, old_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
#endif

    AVBPrint buf;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    const int end = qr->width + QR_MARGIN;

    for (int y = -QR_MARGIN; y < end; y += QR_CELL_H) {
        int last_swap = -1;

        for (int x = -QR_MARGIN; x < end; x += QR_CELL_W) {
            int mask = 0;
            for (int i = 0; i < QR_CELL_W*QR_CELL_H; i++)
                mask |= qr_module(qr, x + (i % QR_CELL_W), y + (i / QR_CELL_W)) << i;

            /* Fall back to the complement when the pattern has no glyph */
            int swap = !qr_octants[mask];
            if (swap)
                mask ^= 0xFF;

            /* The glyph paints set modules in the foreground colour */
            if (swap != last_swap) {
                av_bprintf(&buf, "\033[38;5;%s;48;5;%sm",
                           swap ? QR_COL_CLEAR : QR_COL_SET,
                           swap ? QR_COL_SET : QR_COL_CLEAR);
                last_swap = swap;
            }

            qr_put_utf8(&buf, qr_octants[mask]);
        }

        av_bprintf(&buf, "\033[0m\n");
    }

    if (av_bprint_is_complete(&buf)) {
        fputs(buf.str, stdout);
        fflush(stdout);
    }

    av_bprint_finalize(&buf, NULL);
    QRcode_free(qr);

#ifdef _WIN32
    SetConsoleOutputCP(old_cp);
    if (mode_changed)
        SetConsoleMode(con, old_mode);
#endif
#endif
}
