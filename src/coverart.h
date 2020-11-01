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

#include "cyanrip_main.h"

int crip_fill_coverart(cyanrip_ctx *ctx, int info_only);
int crip_fill_track_coverart(cyanrip_ctx *ctx, int info_only);
int crip_save_art(cyanrip_ctx *ctx, CRIPArt *art, const cyanrip_out_fmt *fmt);
void crip_free_art(CRIPArt *art);
