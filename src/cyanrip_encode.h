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

typedef struct cyanrip_dec_ctx cyanrip_dec_ctx;
typedef struct cyanrip_enc_ctx cyanrip_enc_ctx;

void cyanrip_print_codecs(void);
int cyanrip_validate_fmt(const char *fmt);
const char *cyanrip_fmt_desc(enum cyanrip_output_formats format);
const char *cyanrip_fmt_folder(enum cyanrip_output_formats format);

int cyanrip_create_dec_ctx(cyanrip_ctx *ctx, cyanrip_dec_ctx **s,
                           cyanrip_track *t);
int cyanrip_init_track_encoding(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                                cyanrip_track *t, enum cyanrip_output_formats format);
int cyanrip_send_pcm_to_encoders(cyanrip_ctx *ctx, cyanrip_enc_ctx **enc_ctx,
                                 int num_enc, cyanrip_dec_ctx *dec_ctx,
                                 const uint8_t *data, int bytes);

int cyanrip_reset_encoding(cyanrip_ctx *ctx, cyanrip_track *t);
int cyanrip_finalize_encoding(cyanrip_ctx *ctx, cyanrip_track *t);

int cyanrip_initialize_ebur128(cyanrip_ctx *ctx);
int cyanrip_finalize_ebur128(cyanrip_ctx *ctx, int log);

int cyanrip_writeout_track(cyanrip_ctx *ctx, cyanrip_enc_ctx *enc_ctx);

int cyanrip_end_track_encoding(cyanrip_enc_ctx **s);
void cyanrip_free_dec_ctx(cyanrip_ctx *ctx, cyanrip_dec_ctx **s);
