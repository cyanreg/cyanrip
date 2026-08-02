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

#pragma once

/* Test-only seam: the pregap search algorithm's dependencies on libcdio's
 * track metadata queries are indirected through this ops table so unit tests
 * can supply a synthetic disc layout instead of a real one. Production code
 * only ever uses cyanrip_get_track_pregap_lsn() in pregap.h; this header
 * exists for tests/pregap_test.c.
 *
 * cyanrip_read_audio_subq_sectors() (the actual Q sub-channel read) is
 * deliberately NOT part of this table: it's our own symbol (implemented per
 * platform, not a libcdio shared-library export), so a test can substitute
 * it directly by providing its own definition and not linking
 * subq_read_mmc.c/subq_read_macos.c - no ops indirection needed for it.
 *
 * The callback signatures match libcdio's real signatures exactly (const
 * CdIo_t * first argument) so production can assign them directly with no
 * adapter thunks; the algorithm itself takes the disc pointer from
 * ctx->cdio at each call site.
 */

#include <stdint.h>
#include <cdio/cdio.h>
#include "cyanrip_main.h"

typedef struct cyanrip_pregap_ops {
    lsn_t          (*get_track_pregap_lsn)(const CdIo_t *p_cdio, track_t track_number);
    track_t        (*get_first_track_num)(const CdIo_t *p_cdio);
    lsn_t          (*get_track_lsn)(const CdIo_t *p_cdio, track_t track_number);
    track_format_t (*get_track_format)(const CdIo_t *p_cdio, track_t track_number);
} cyanrip_pregap_ops;

lsn_t cyanrip_get_track_pregap_lsn_impl(cyanrip_ctx *ctx, const cyanrip_pregap_ops *ops,
                                         track_t track_number);
