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

/* Runs the pregap search against a mock disc: the cdio_* calls and
 * cyanrip_read_audio_subchannel_sector() are defined here, libcdio isn't linked. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "pregap.h"
#include "cyanrip_main.h"
#include "cyanrip_log.h"
#include "subq_read.h"

void cyanrip_log(cyanrip_ctx *ctx, int verbose, const char *format, ...)
{
    (void)ctx;
    (void)verbose;
    (void)format;
}

static int fails = 0;

#define MAX_FAULTS 24
#define MAX_JITTER 4
#define MAX_MODE2 4

typedef struct {
    lsn_t lsn;
    int remaining; /* <0: always bad; >0: bad for this many reads, then good */
} lsn_fault_t;

typedef struct {
    lsn_t lsn;
    lsn_t reports_as;
} lsn_jitter_t;

typedef struct {
    track_t first_track_num;
    track_t prev_track_number;
    track_t cur_track_number;
    lsn_t prev_track_start_lsn;
    lsn_t cur_pregap_start_lsn;
    lsn_t cur_track_start_lsn;
    track_format_t prev_track_format;
    track_format_t cur_track_format;
    int simulate_libcdio_pregap_support;
    int nonbcd;
    int nocrc; /* drive hands back the formatted Q without its CRC */
    int no_raw_pw; /* drive can't read raw P-W: the formatted Q is all there is */
    int raw_pw_garbage; /* drive accepts raw P-W reads but returns zeros */
    lsn_t stale; /* formatted Q returns the previous frame, raw P-W the damaged one */
    int stale_damage; /* 0: spare byte, 1: one bit of absolute time, 2: both */
    int q_offset; /* Q sub-channel runs this many sectors ahead of the TOC */
    lsn_t ctx_start_lsn; /* ctx->start_lsn, for the first track */
    lsn_t read_error; /* reads of this sector fail outright, not just its CRC */

    lsn_fault_t faults[MAX_FAULTS];
    int num_faults;
    lsn_jitter_t jitter[MAX_JITTER];
    int num_jitter;
    lsn_t mode2[MAX_MODE2]; /* catalogue number frames */
    int num_mode2;

    int reads_issued;
    enum cyanrip_subq_read_mode mode_chosen;
    cyanrip_pregap_info info;
} fake_disc_t;

static fake_disc_t disc;

static void make_disc(lsn_t prev_start, lsn_t pregap_start, lsn_t cur_start)
{
    memset(&disc, 0, sizeof(disc));
    disc.stale = CDIO_INVALID_LSN;
    disc.read_error = CDIO_INVALID_LSN;
    disc.first_track_num = 1;
    disc.prev_track_number = 5;
    disc.cur_track_number = 6;
    disc.prev_track_start_lsn = prev_start;
    disc.cur_pregap_start_lsn = pregap_start;
    disc.cur_track_start_lsn = cur_start;
    disc.prev_track_format = TRACK_FORMAT_AUDIO;
    disc.cur_track_format = TRACK_FORMAT_AUDIO;
}

/* ---- Q frame generation ---- */

static unsigned test_crc_subq(const uint8_t *q)
{
    int length = 10;
    const unsigned crc_poly = 0x1021;
    unsigned r = 0x0000;
    const uint8_t *p = q;
    while (length--) {
        r ^= *p++ << 8;
        for (int i = 0; i < 8; i++)
            r = r & 0x8000 ? (r << 1) ^ crc_poly : r << 1;
    }
    return ~r & 0xFFFF;
}

static uint8_t bin_to_bcd(uint8_t x)
{
    return (uint8_t)(((x / 10) << 4) | (x % 10));
}

static uint8_t bcd_to_bin(uint8_t x)
{
    return (uint8_t)(10 * ((x & 0xF0) >> 4) + (x & 0x0F));
}

static void true_subq_at(lsn_t content_lsn, track_t *out_track, uint8_t *out_index)
{
    if (content_lsn >= disc.cur_pregap_start_lsn) {
        *out_track = disc.cur_track_number;
        *out_index = content_lsn >= disc.cur_track_start_lsn ? 1 : 0;
    } else {
        *out_track = disc.prev_track_number;
        *out_index = 1;
    }
}

/* Returns 0 for a faulted sector, leaving the frame zeroed */
static int fake_subq_frame(lsn_t lsn, uint8_t *q)
{
    memset(q, 0, 16);

    for (int i = 0; i < disc.num_faults; i++) {
        if (disc.faults[i].lsn != lsn)
            continue;
        if (disc.faults[i].remaining < 0)
            return 0;
        if (disc.faults[i].remaining > 0) {
            disc.faults[i].remaining--;
            return 0;
        }
        break;
    }

    lsn_t content_lsn = lsn + disc.q_offset;
    for (int i = 0; i < disc.num_jitter; i++) {
        if (disc.jitter[i].lsn == lsn) {
            content_lsn = disc.jitter[i].reports_as;
            break;
        }
    }

    track_t true_track;
    uint8_t true_index;
    true_subq_at(content_lsn, &true_track, &true_index);

    /* control=0b0001 (2ch audio, no pre-emphasis), adr=1 (position data) */
    q[0] = (0x1 << 4) | 0x1;
    for (int i = 0; i < disc.num_mode2; i++) {
        if (disc.mode2[i] == lsn) {
            /* adr=2: only the adr and CRC are looked at */
            q[0] = (0x1 << 4) | 0x2;
            break;
        }
    }
    q[1] = bin_to_bcd(true_track);
    q[2] = bin_to_bcd(true_index);
    q[3] = bin_to_bcd(0);
    q[4] = bin_to_bcd(12); /* >= 10 so BCD and binary differ */
    q[5] = bin_to_bcd(0);
    q[6] = 0;
    const lsn_t abs_frames = content_lsn + CDIO_PREGAP_SECTORS;
    q[7] = bin_to_bcd((uint8_t)(abs_frames / (60 * 75)));
    q[8] = bin_to_bcd((uint8_t)(abs_frames / 75 % 60));
    q[9] = bin_to_bcd((uint8_t)(abs_frames % 75));

    unsigned crc = test_crc_subq(q);
    q[10] = (crc >> 8) & 0xFF;
    q[11] = crc & 0xFF;
    return 1;
}

static driver_return_code_t fake_read_subq(uint8_t *buf, lsn_t lsn)
{
    disc.reads_issued++;
    if (lsn == disc.read_error)
        return DRIVER_OP_NOT_PERMITTED;

    uint8_t *q = buf + CDIO_CD_FRAMESIZE_RAW;

    if (lsn == disc.stale)
        lsn -= 1;

    if (!fake_subq_frame(lsn, q))
        return DRIVER_OP_SUCCESS;

    if (disc.nonbcd) {
        /* Binary instead of BCD, CRC computed over the BCD bytes */
        static const int fields[] = { 1, 2, 3, 4, 5, 7, 8, 9 };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
            q[fields[i]] = bcd_to_bin(q[fields[i]]);
    }
    if (disc.nocrc)
        q[10] = q[11] = 0;
    return DRIVER_OP_SUCCESS;
}

/* Raw P-W: Q in bit 6, P set throughout */
static driver_return_code_t fake_read_subpw(uint8_t *buf, lsn_t lsn)
{
    if (disc.no_raw_pw)
        return DRIVER_OP_UNSUPPORTED;

    disc.reads_issued++;
    if (lsn == disc.read_error)
        return DRIVER_OP_NOT_PERMITTED;

    uint8_t *pw = buf + CDIO_CD_FRAMESIZE_RAW;
    memset(pw, 0, CDIO_CD_FRAMESIZE_SUB);
    if (disc.raw_pw_garbage)
        return DRIVER_OP_SUCCESS;

    uint8_t q[16];
    fake_subq_frame(lsn, q);
    if (lsn == disc.stale) {
        if (disc.stale_damage != 1)
            q[6] ^= 0x10; /* a bit error the payload survives */
        if (disc.stale_damage != 0)
            q[9] ^= 0x20; /* a bit error in the absolute time: 20 frames off */
    }

    for (int i = 0; i < CDIO_CD_FRAMESIZE_SUB; i++)
        pw[i] = 0x80 | (((q[i >> 3] >> (7 - (i & 7))) & 1) << 6);
    return DRIVER_OP_SUCCESS;
}

driver_return_code_t cyanrip_read_audio_subchannel_sector(const CdIo_t *p_cdio, uint8_t *buf, lsn_t lsn,
                                                          enum cyanrip_subchannel subchannel, size_t block_size)
{
    (void)p_cdio;
    switch (subchannel) {
    case CYANRIP_SUBCHANNEL_Q:
        if (block_size != CYANRIP_CD_FRAMESIZE_RAW_AND_SUBQ)
            return DRIVER_OP_BAD_PARAMETER;
        return fake_read_subq(buf, lsn);
    case CYANRIP_SUBCHANNEL_PW_RAW:
        if (block_size != CYANRIP_CD_FRAMESIZE_RAW_AND_SUBPW)
            return DRIVER_OP_BAD_PARAMETER;
        return fake_read_subpw(buf, lsn);
    default:
        return DRIVER_OP_BAD_PARAMETER;
    }
}

lsn_t cdio_get_track_pregap_lsn(const CdIo_t *p_cdio, track_t track_number)
{
    (void)p_cdio;
    if (!disc.simulate_libcdio_pregap_support)
        return CDIO_INVALID_LSN;
    return track_number == disc.cur_track_number ? disc.cur_pregap_start_lsn : CDIO_INVALID_LSN;
}

track_t cdio_get_first_track_num(const CdIo_t *p_cdio)
{
    (void)p_cdio;
    return disc.first_track_num;
}

lsn_t cdio_get_track_lsn(const CdIo_t *p_cdio, track_t track_number)
{
    (void)p_cdio;
    if (track_number == disc.cur_track_number)
        return disc.cur_track_start_lsn;
    if (track_number == disc.prev_track_number)
        return disc.prev_track_start_lsn;
    return CDIO_INVALID_LSN;
}

track_format_t cdio_get_track_format(const CdIo_t *p_cdio, track_t track_number)
{
    (void)p_cdio;
    if (track_number == disc.cur_track_number)
        return disc.cur_track_format;
    if (track_number == disc.prev_track_number)
        return disc.prev_track_format;
    return TRACK_FORMAT_ERROR;
}

static lsn_t run(void)
{
    cyanrip_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.start_lsn = disc.ctx_start_lsn;
    disc.reads_issued = 0;
    lsn_t got = cyanrip_get_track_pregap_lsn(&ctx, disc.cur_track_number, &disc.info);
    disc.mode_chosen = ctx.subq_read_mode;
    return got;
}

static void check_lsn(const char *what, lsn_t got, lsn_t want)
{
    if (got != want) {
        printf("FAIL: %s: got %d, want %d\n", what, got, want);
        fails++;
    }
}

static void check_true(const char *what, int cond)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        fails++;
    }
}

int main(void)
{
    /* First track: the pregap is the disc's start */
    {
        make_disc(1000, 1150, 1300);
        disc.cur_track_number = disc.first_track_num;
        disc.ctx_start_lsn = disc.cur_track_start_lsn; /* no lead-in gap */
        lsn_t got = run();
        check_lsn("first track, no lead-in gap", got, disc.ctx_start_lsn);
        check_true("first track, no lead-in gap: no subq reads", disc.reads_issued == 0);
    }

    {
        make_disc(1000, 1150, 1300);
        disc.cur_track_number = disc.first_track_num;
        disc.ctx_start_lsn = 0;
        lsn_t got = run();
        check_lsn("first track, lead-in gap", got, disc.ctx_start_lsn);
        check_true("first track, lead-in gap: no subq reads", disc.reads_issued == 0);
    }

    /* libcdio already knows the pregap */
    {
        make_disc(1000, 1150, 1300);
        disc.simulate_libcdio_pregap_support = 1;
        lsn_t got = run();
        check_lsn("libcdio-reported pregap", got, 1150);
        check_true("libcdio-reported pregap: no subq reads", disc.reads_issued == 0);
    }

    /* No pregap: two reads */
    {
        make_disc(1000, 1300, 1300);
        lsn_t got = run();
        check_lsn("no pregap", got, CDIO_INVALID_LSN);
        check_true("no pregap: fast path used only 2 reads", disc.reads_issued == SUBQ_PROBE_SECTORS + 2);
    }

    /* Previous track is a single sector */
    {
        make_disc(1299, 1300, 1300);
        lsn_t got = run();
        check_lsn("single sector previous track", got, CDIO_INVALID_LSN);
        check_true("single sector previous track: no subq reads", disc.reads_issued == 0);
    }

    /* Ordinary ~2s pregap. */
    {
        make_disc(1000, 1150, 1300);
        lsn_t got = run();
        check_lsn("short pregap", got, 1150);
    }

    /* Pregap longer than one backtrack step */
    {
        make_disc(1000, 1500, 2000);
        lsn_t got = run();
        check_lsn("long pregap", got, 1500);
    }

    /* Data track next to the boundary */
    {
        make_disc(1000, 1150, 1300);
        disc.cur_track_format = TRACK_FORMAT_DATA;
        lsn_t got = run();
        check_lsn("data track guard (current)", got, CDIO_INVALID_LSN);
        check_true("data track guard (current): no subq reads", disc.reads_issued == 0);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.prev_track_format = TRACK_FORMAT_DATA;
        lsn_t got = run();
        check_lsn("data track guard (previous)", got, CDIO_INVALID_LSN);
        check_true("data track guard (previous): no subq reads", disc.reads_issued == 0);
    }

    /* Raw P-W when the drive supports it and returns valid frames */
    {
        make_disc(1000, 1150, 1300);
        lsn_t got = run();
        check_lsn("raw P-W drive", got, 1150);
        check_true("raw P-W drive: raw mode chosen", disc.mode_chosen == CYANRIP_SUBQ_READ_RAW_PW);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.no_raw_pw = 1;
        lsn_t got = run();
        check_lsn("drive without raw P-W", got, 1150);
        check_true("drive without raw P-W: formatted mode chosen", disc.mode_chosen == CYANRIP_SUBQ_READ_FORMATTED_Q);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.raw_pw_garbage = 1;
        lsn_t got = run();
        check_lsn("drive returning junk raw P-W", got, 1150);
        check_true("drive returning junk raw P-W: formatted mode chosen", disc.mode_chosen == CYANRIP_SUBQ_READ_FORMATTED_Q);
    }

    /* Undecodable sector at the boundary: formatted Q returns the previous frame,
     * raw P-W the damaged one */
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1150;
        disc.no_raw_pw = 1;
        lsn_t got = run();
        check_lsn("stale formatted frame at the boundary", got, 1151);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1150;
        lsn_t got = run();
        check_lsn("stale frame at the boundary, seen through raw P-W", got, 1150);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1149; /* the last sector of the previous track instead */
        lsn_t got = run();
        check_lsn("damaged frame just below the boundary", got, 1150);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1150;
        disc.q_offset = 2;
        lsn_t got = run();
        check_lsn("damaged frame at the boundary, Q ahead of the TOC", got, 1150);
    }

    /* Single bit errors are repaired, two are not */
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1150;
        disc.stale_damage = 1;
        lsn_t got = run();
        check_lsn("damaged frame with a single bit error is repaired", got, 1150);
        check_true("damaged frame with a single bit error is repaired: reported",
                   disc.info.result == CYANRIP_PREGAP_SEARCH_DONE && disc.info.damaged_frames == 1 && disc.info.repaired_frames == 1);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1150;
        disc.stale_damage = 2;
        lsn_t got = run();
        check_lsn("damaged frame with two bit errors gives up", got, CDIO_INVALID_LSN);
        check_true("damaged frame with two bit errors gives up: reported",
                   disc.info.result == CYANRIP_PREGAP_SEARCH_UNREADABLE);
    }

    /* Stale sector away from the boundary */
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1200;
        lsn_t got = run();
        check_lsn("stale frame inside the pregap", got, 1150);
    }

    /* Binary instead of BCD */
    {
        make_disc(1000, 1150, 1300);
        disc.no_raw_pw = 1;
        disc.nonbcd = 1;
        lsn_t got = run();
        check_lsn("non-BCD drive quirk", got, 1150);
    }

    /* No CRC in the formatted Q */
    {
        make_disc(1000, 1150, 1300);
        disc.no_raw_pw = 1;
        disc.nocrc = 1;
        lsn_t got = run();
        check_lsn("drive without Q CRC", got, 1150);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.no_raw_pw = 1;
        disc.nocrc = 1;
        disc.nonbcd = 1;
        lsn_t got = run();
        check_lsn("non-BCD drive without Q CRC", got, 1150);
    }
    {
        make_disc(1000, 1300, 1300);
        disc.no_raw_pw = 1;
        disc.nocrc = 1;
        lsn_t got = run();
        check_lsn("drive without Q CRC, no pregap", got, CDIO_INVALID_LSN);
    }

    /* No CRC, wrong sector rejected by its absolute time */
    {
        make_disc(1000, 1300, 1500);
        disc.no_raw_pw = 1;
        disc.nocrc = 1;
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1250, .reports_as = 1305 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("drive without Q CRC: wrong sector read is rejected", got, 1300);
    }

    /* No CRC and Q 2 sectors early */
    {
        make_disc(1000, 1150, 1300);
        disc.no_raw_pw = 1;
        disc.nocrc = 1;
        disc.q_offset = 2;
        lsn_t got = run();
        check_lsn("drive without Q CRC, Q ahead of the TOC", got, 1150);
    }

    /* Spurious read of the new track */
    {
        make_disc(1000, 1300, 1500);
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1250, .reports_as = 1305 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("single spurious read is not trusted", got, 1300);
    }

    /* Spurious read of the previous track */
    {
        make_disc(1000, 1100, 2000);
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1249, .reports_as = 1050 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("single spurious prev-track read is not trusted", got, 1100);
    }

    /* Dead sector away from the boundary */
    {
        make_disc(1000, 1200, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1149, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("bad sector away from boundary is skipped", got, 1200);
    }

    /* One sector pregap, confirmed by the track start */
    {
        make_disc(1000, 1299, 1300);
        lsn_t got = run();
        check_lsn("one sector pregap", got, 1299);
    }

    /* No pregap, Q ahead of the TOC */
    {
        make_disc(1000, 1300, 1300);
        disc.q_offset = 2;
        lsn_t got = run();
        check_lsn("no pregap, Q ahead of the TOC", got, CDIO_INVALID_LSN);
    }

    /* Pregap, Q ahead of the TOC */
    {
        make_disc(1000, 1150, 1300);
        disc.q_offset = 2;
        lsn_t got = run();
        check_lsn("pregap, Q ahead of the TOC", got, 1150);
        check_true("pregap, Q ahead of the TOC: skew reported", disc.info.q_skew == 2);
    }

    /* Pregap, Q behind the TOC */
    {
        make_disc(1000, 1150, 1300);
        disc.q_offset = -2;
        lsn_t got = run();
        check_lsn("pregap, Q behind the TOC", got, 1150);
    }

    /* Bounds disagree on absolute time, keep the sector asked for */
    {
        make_disc(1000, 1150, 1300);
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1150, .reports_as = 1153 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("unsteady Q skew at the boundary", got, 1150);
    }

    /* Mode 2 frame in the pregap */
    {
        make_disc(1000, 1150, 1300);
        disc.mode2[0] = 1151;
        disc.num_mode2 = 1;
        lsn_t got = run();
        check_lsn("mode 2 Q frame right after the boundary", got, 1150);
    }

    /* Dead sector, and both back to back */
    {
        make_disc(1000, 1150, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1151, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("dead sector right after the boundary", got, 1150);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.mode2[0] = 1151;
        disc.num_mode2 = 1;
        disc.faults[0] = (lsn_fault_t){ .lsn = 1152, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("mode 2 Q frame and dead sector right after the boundary", got, 1150);
    }

    /* Two sector pregap with the second dead */
    {
        make_disc(1000, 1298, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1299, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("two sector pregap, second one dead", got, 1298);
    }

    /* Spurious read followed by a dead sector */
    {
        make_disc(1000, 1300, 1500);
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1250, .reports_as = 1305 };
        disc.num_jitter = 1;
        disc.faults[0] = (lsn_fault_t){ .lsn = 1251, .remaining = -1 };
        disc.num_faults = 1;
        disc.mode2[0] = 1252;
        disc.num_mode2 = 1;
        lsn_t got = run();
        check_lsn("spurious read followed by silent sectors is not trusted", got, 1300);
    }

    /* Dead sector in a long scanned range */
    {
        make_disc(1000, 1500, 2000);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1450, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("bad sector inside the scanned range is skipped", got, 1500);
    }

    /* Flaky sector at the boundary */
    {
        make_disc(1000, 1150, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1150, .remaining = 3 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("flaky boundary sector recovers via retries", got, 1150);
    }

    /* Dead sector at the boundary */
    {
        make_disc(1000, 1150, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1150, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("permanently dead boundary sector gives up", got, CDIO_INVALID_LSN);
    }

    /* Many dead sectors, the failure budget ends the search */
    {
        make_disc(1000, 1150, 1300);
        disc.num_faults = MAX_FAULTS;
        for (int i = 0; i < MAX_FAULTS; i++)
            disc.faults[i] = (lsn_fault_t){ .lsn = 1140 + i, .remaining = -1 };
        lsn_t got = run();
        check_lsn("wide dead zone gives up", got, CDIO_INVALID_LSN);
        check_true("failure budget bounds the read count", disc.reads_issued < 2000);
    }

    /* Read errors on the confirming reads report the sector that failed */
    {
        make_disc(1000, 1300, 1300);
        disc.read_error = 1298;
        lsn_t got = run();
        check_lsn("read error confirming no pregap", got, CDIO_INVALID_LSN);
        check_true("read error confirming no pregap: reported",
                   disc.info.result == CYANRIP_PREGAP_SEARCH_READ_ERROR &&
                   disc.info.failed_error == DRIVER_OP_NOT_PERMITTED);
        check_lsn("read error confirming no pregap: failed LSN", disc.info.failed_lsn, 1298);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.read_error = 1148;
        lsn_t got = run();
        check_lsn("read error confirming the left bound", got, CDIO_INVALID_LSN);
        check_lsn("read error confirming the left bound: failed LSN", disc.info.failed_lsn, 1148);
    }
    {
        make_disc(1000, 1100, 1300);
        disc.read_error = 1150;
        lsn_t got = run();
        check_lsn("read error confirming the right bound", got, CDIO_INVALID_LSN);
        check_lsn("read error confirming the right bound: failed LSN", disc.info.failed_lsn, 1150);
    }

    if (fails) {
        printf("%i check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
