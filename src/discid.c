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

#include "discid.h"
#include "cyanrip_log.h"

#include <discid/discid.h>

int crip_fill_discid(cyanrip_ctx *ctx)
{
    if (ctx->mcap & CDIO_DRIVE_CAP_MISC_FILE)
        return 0;

    /* Get discid */
    DiscId *discid = discid_new();
    if (!discid_read_sparse(discid, ctx->settings.dev_path, 0)) {
        cyanrip_log(ctx, 0, "Error reading discid: %s!\n",
                    discid_get_error_msg(discid));
        return 1;
    }

    /* Set metadata */
    const char *disc_id_str = discid_get_id(discid);
    av_dict_set(&ctx->meta, "discid", disc_id_str, 0);
    discid_free(discid);

    return 0;
}
