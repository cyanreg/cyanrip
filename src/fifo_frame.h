#pragma once

#include <pthread.h>
#include <assert.h>
#include <libavutil/frame.h>

enum CRFrameFIFOFlags {
    FRAME_FIFO_BLOCK_MAX_OUTPUT = (1 << 0),
    FRAME_FIFO_BLOCK_NO_INPUT   = (1 << 1),
};

#define FRENAME(x) FRAME_FIFO_ ## x
#define RENAME(x)  cr_frame_ ##x
#define FNAME      enum CRFrameFIFOFlags
#define TYPE       AVFrame

#include "fifo_template.h"

#undef TYPE
#undef FNAME
#undef RENAME
#undef FRENAME
