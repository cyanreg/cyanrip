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

#include "cyanrip_encode.h"

#include <libavcodec/avcodec.h>

static const char *extensions[] = { "flac", "wv", "tta", "alac", "opus", "ogg", "mp3", "wav", "dat" };

static int avcodec_init(void)
{
    int result;

    AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!enc) {
        av_log(NULL, AV_LOG_ERROR, "Can't find encoder\n");
        return 1;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(enc);

    ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    ctx->sample_rate = 44100;
    ctx->channel_layout = AV_CH_LAYOUT_STEREO;

    result = avcodec_open2(ctx, enc, NULL);
    if (result < 0) {
        av_log(ctx, AV_LOG_ERROR, "Can't open encoder\n");
        return result;
    }
}

static int cyanrip_encode_format(cyanrip_ctx *ctx, int idx)
{
    char filename[255];

    sprintf(filename, "%s.%s", ctx->tracks[ctx->cur_track].name, extensions[ctx->settings.output_formats[idx]]);

    FILE *output = fopen(filename, "wb");

    if (ctx->settings.output_formats[idx] == CYANRIP_FORMAT_RAW) {
        fwrite(ctx->samples, sizeof(int16_t), ctx->samples_num, output);
    } else if (ctx->settings.output_formats[idx] == CYANRIP_FORMAT_WAV) {

        uint8_t header[] = { 
    0x52, 0x49, 0x46, 0x46, 0x24, 0x40, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20,
    0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x44, 0xac, 0x00, 0x00, 0x10, 0xb1, 0x02, 0x00,
    0x04, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61, 0xff, 0xff, 0xff, 0xff
  };  

        fwrite(header, sizeof(uint8_t), sizeof(header), output);

        fwrite(ctx->samples, sizeof(int16_t), ctx->samples_num, output);
    }

    fclose(output);

    return 0;
}

int cyanrip_encode_track(cyanrip_ctx *ctx)
{
    for (int i = 0; i < ctx->settings.outputs_number; i++)
        cyanrip_encode_format(ctx, i);
    return 0;
}
