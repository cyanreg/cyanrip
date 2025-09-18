#pragma once

#include <cdio/cdio.h>

lsn_t crip_get_track_pregap_lsn(CdIo_t *p_cdio, track_t i_track);
