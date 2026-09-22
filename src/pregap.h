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

#include <cdio/cdio.h>
#include "cyanrip_main.h"

/* Sectors read to find out whether the drive's raw P-W sub-channel is usable */
#define SUBQ_PROBE_SECTORS 10

/**
 * Finds the sector the pregap of track_number starts at, or CDIO_INVALID_LSN
 * if it has none or it couldn't be found. info, if given, says how the search
 * went; see cyanrip_pregap_info.
 */
lsn_t cyanrip_get_track_pregap_lsn(cyanrip_ctx *ctx, track_t track_number, cyanrip_pregap_info *info);
