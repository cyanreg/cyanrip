#pragma once

#include <cdio/cdio.h>

lba_t cyanrip_get_track_pregap_lba(CdIo_t *p_cdio, track_t i_track);
lsn_t cyanrip_get_track_pregap_lsn(CdIo_t *p_cdio, track_t i_track);
