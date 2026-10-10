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

#include "cyanrip_main.h"

/* Whether tracks get fingerprinted: the build has libchromaprint, and the
 * user hasn't disabled it */
int crip_acoustid_enabled(cyanrip_ctx *ctx);

/* Restarts the fingerprint, at the start of every pass over a track. The
 * fingerprint covers the first 2 minutes of the pass, as fpcalc's does. */
int crip_init_fingerprint(cyanrip_ctx *ctx);
void crip_process_fingerprint(cyanrip_ctx *ctx, const uint8_t *data, int bytes);
void crip_finalize_fingerprint(cyanrip_ctx *ctx, cyanrip_track *t);

/* Once the disc is ripped and before the files are written: if every track
 * is accurately ripped and has a MusicBrainz recording, submits the
 * fingerprints to AcoustID, otherwise tags the files with them. */
void crip_acoustid_finish(cyanrip_ctx *ctx);

void crip_free_fingerprint(cyanrip_ctx *ctx);
