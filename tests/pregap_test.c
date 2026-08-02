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

/* Exercises the Q sub-channel pregap search in pregap.c against a synthetic
 * drive (no real libcdio driver or hardware involved), via the ops-table
 * seam declared in pregap_internal.h. This lets us inject drive misbehaviour
 * (spurious reads, permanently bad sectors, BCD-quirk drives) that would be
 * impractical to reproduce with real hardware or a bin/cue image.
 *
 * The ops callbacks take a cyanrip_ctx*, matching production, but the fake
 * disc being ripped is described by a separate, test-only fixture (g_disc)
 * that the callbacks read from directly: cyanrip_ctx has no room for a
 * synthetic track layout or fault injection, and shouldn't grow any just for
 * this. The ctx itself is only actually used for ctx->subq_needs_bcd_fixup.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "pregap.h"
#include "pregap_internal.h"
#include "cyanrip_main.h"
#include "cyanrip_log.h"

/* pregap.c logs failures via cyanrip_log(); give it somewhere to go. */
void cyanrip_log(cyanrip_ctx *ctx, int verbose, const char *format, ...)
{
    (void)ctx;
    (void)verbose;
    (void)format;
}

static int fails = 0;

#define MAX_FAULTS 24
#define MAX_JITTER 4

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

    lsn_fault_t faults[MAX_FAULTS];
    int num_faults;
    lsn_jitter_t jitter[MAX_JITTER];
    int num_jitter;

    int reads_issued;
} fake_disc_t;

/* The disc/drive the fake ops callbacks below currently serve. Set by run()
 * before each scenario; only one scenario is ever in flight at a time. */
static fake_disc_t *g_disc;

static fake_disc_t make_disc(lsn_t prev_start, lsn_t pregap_start, lsn_t cur_start)
{
    fake_disc_t d;
    memset(&d, 0, sizeof(d));
    d.first_track_num = 1;
    d.prev_track_number = 5;
    d.cur_track_number = 6;
    d.prev_track_start_lsn = prev_start;
    d.cur_pregap_start_lsn = pregap_start;
    d.cur_track_start_lsn = cur_start;
    d.prev_track_format = TRACK_FORMAT_AUDIO;
    d.cur_track_format = TRACK_FORMAT_AUDIO;
    return d;
}

/* ---- Q sub-channel fixture generation: duplicates pregap.c's CRC-16 and BCD
 * encoding just enough to build self-consistent synthetic sectors. ---- */

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

/* True (track_number, index_number) for a physical position, given a disc
 * with exactly one pregap boundary of interest (prev track's tail and the
 * current track's pregap/start). */
static void true_subq_at(const fake_disc_t *d, lsn_t content_lsn,
                          track_t *out_track, uint8_t *out_index)
{
    if (content_lsn >= d->cur_pregap_start_lsn) {
        *out_track = d->cur_track_number;
        *out_index = content_lsn >= d->cur_track_start_lsn ? 1 : 0;
    } else {
        *out_track = d->prev_track_number;
        *out_index = 1;
    }
}

static driver_return_code_t fake_read_audio_subq_sectors(const CdIo_t *p_cdio, uint8_t *buf,
                                                          lsn_t lsn, uint32_t blocks)
{
    fake_disc_t *d = g_disc;
    (void)p_cdio;
    (void)blocks;
    d->reads_issued++;

    uint8_t *q = buf + CDIO_CD_FRAMESIZE_RAW;
    memset(q, 0, 16);

    for (int i = 0; i < d->num_faults; i++) {
        if (d->faults[i].lsn != lsn)
            continue;
        if (d->faults[i].remaining < 0)
            return DRIVER_OP_SUCCESS; /* all-zero sector: CRC field 0, always invalid */
        if (d->faults[i].remaining > 0) {
            d->faults[i].remaining--;
            return DRIVER_OP_SUCCESS;
        }
        break;
    }

    lsn_t content_lsn = lsn;
    for (int i = 0; i < d->num_jitter; i++) {
        if (d->jitter[i].lsn == lsn) {
            content_lsn = d->jitter[i].reports_as;
            break;
        }
    }

    track_t true_track;
    uint8_t true_index;
    true_subq_at(d, content_lsn, &true_track, &true_index);

    /* control=0b0001 (2ch audio, no pre-emphasis), adr=1 (position data) */
    q[0] = (0x1 << 4) | 0x1;
    q[1] = bin_to_bcd(true_track);
    q[2] = bin_to_bcd(true_index);
    q[3] = bin_to_bcd(0);
    q[4] = bin_to_bcd(12); /* relative seconds: >=10 so BCD vs binary actually differ */
    q[5] = bin_to_bcd(0);
    q[6] = 0;
    q[7] = bin_to_bcd(0);
    q[8] = bin_to_bcd(0);
    q[9] = bin_to_bcd(0);

    if (d->nonbcd) {
        /* Simulate a drive whose firmware hands back raw binary values
         * instead of BCD for these fields (the on-disc/true CRC below is
         * still computed over the correct BCD bytes first). */
        static const int fields[] = { 1, 2, 3, 4, 5, 7, 8, 9 };
        unsigned crc = test_crc_subq(q);
        q[10] = (crc >> 8) & 0xFF;
        q[11] = crc & 0xFF;
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
            q[fields[i]] = bcd_to_bin(q[fields[i]]);
        return DRIVER_OP_SUCCESS;
    }

    unsigned crc = test_crc_subq(q);
    q[10] = (crc >> 8) & 0xFF;
    q[11] = crc & 0xFF;
    return DRIVER_OP_SUCCESS;
}

static lsn_t fake_get_track_pregap_lsn(const CdIo_t *p_cdio, track_t track_number)
{
    fake_disc_t *d = g_disc;
    (void)p_cdio;
    if (!d->simulate_libcdio_pregap_support)
        return CDIO_INVALID_LSN;
    return track_number == d->cur_track_number ? d->cur_pregap_start_lsn : CDIO_INVALID_LSN;
}

static track_t fake_get_first_track_num(const CdIo_t *p_cdio)
{
    (void)p_cdio;
    return g_disc->first_track_num;
}

static lsn_t fake_get_track_lsn(const CdIo_t *p_cdio, track_t track_number)
{
    fake_disc_t *d = g_disc;
    (void)p_cdio;
    if (track_number == d->cur_track_number)
        return d->cur_track_start_lsn;
    if (track_number == d->prev_track_number)
        return d->prev_track_start_lsn;
    return CDIO_INVALID_LSN;
}

static track_format_t fake_get_track_format(const CdIo_t *p_cdio, track_t track_number)
{
    fake_disc_t *d = g_disc;
    (void)p_cdio;
    if (track_number == d->cur_track_number)
        return d->cur_track_format;
    if (track_number == d->prev_track_number)
        return d->prev_track_format;
    return TRACK_FORMAT_ERROR;
}

static const cyanrip_pregap_ops fake_ops = {
    .get_track_pregap_lsn    = fake_get_track_pregap_lsn,
    .get_first_track_num     = fake_get_first_track_num,
    .get_track_lsn           = fake_get_track_lsn,
    .get_track_format        = fake_get_track_format,
    .read_audio_subq_sectors = fake_read_audio_subq_sectors,
};

static lsn_t run(fake_disc_t *d)
{
    cyanrip_ctx ctx;
    memset(&ctx, 0, sizeof(ctx)); /* fresh ctx each time: subq_needs_bcd_fixup starts at 0 */
    g_disc = d;
    d->reads_issued = 0;
    return cyanrip_get_track_pregap_lsn_impl(&ctx, &fake_ops, d->cur_track_number);
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
    /* First track: pregap is always lsn 0, no subq work at all. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.cur_track_number = d.first_track_num;
        lsn_t got = run(&d);
        check_lsn("first track", got, 0);
        check_true("first track: no subq reads", d.reads_issued == 0);
    }

    /* libcdio already knows the pregap (e.g. a cue sheet): use it directly,
     * no subq reads needed at all. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.simulate_libcdio_pregap_support = 1;
        lsn_t got = run(&d);
        check_lsn("libcdio-reported pregap", got, 1150);
        check_true("libcdio-reported pregap: no subq reads", d.reads_issued == 0);
    }

    /* No pregap at all: fast path should confirm with just two reads. */
    {
        fake_disc_t d = make_disc(1000, 1300, 1300);
        lsn_t got = run(&d);
        check_lsn("no pregap", got, 1300);
        check_true("no pregap: fast path used only 2 reads", d.reads_issued == 2);
    }

    /* Ordinary ~2s pregap. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        lsn_t got = run(&d);
        check_lsn("short pregap", got, 1150);
    }

    /* Long pregap spanning several 150-sector backtrack jumps. */
    {
        fake_disc_t d = make_disc(1000, 1500, 2000);
        lsn_t got = run(&d);
        check_lsn("long pregap", got, 1500);
    }

    /* Data track adjacent to the boundary: must bail out immediately
     * without doing any subq work at all. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.cur_track_format = TRACK_FORMAT_DATA;
        lsn_t got = run(&d);
        check_lsn("data track guard (current)", got, CDIO_INVALID_LSN);
        check_true("data track guard (current): no subq reads", d.reads_issued == 0);
    }
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.prev_track_format = TRACK_FORMAT_DATA;
        lsn_t got = run(&d);
        check_lsn("data track guard (previous)", got, CDIO_INVALID_LSN);
        check_true("data track guard (previous): no subq reads", d.reads_issued == 0);
    }

    /* A drive whose firmware returns raw binary MSF fields instead of BCD
     * must still resolve the pregap correctly once the quirk is detected. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.nonbcd = 1;
        lsn_t got = run(&d);
        check_lsn("non-BCD drive quirk", got, 1150);
    }

    /* A single spuriously CRC-valid read for the wrong physical sector (seek
     * jitter near the boundary) must not be trusted on its own: the sector
     * right after it will contradict it and the search must still land on
     * the true boundary, not the jittered one. */
    {
        fake_disc_t d = make_disc(1000, 1300, 1500);
        d.jitter[0] = (lsn_jitter_t){ .lsn = 1250, .reports_as = 1305 };
        d.num_jitter = 1;
        lsn_t got = run(&d);
        check_lsn("single spurious read is not trusted", got, 1300);
    }

    /* A sector that's permanently unreadable but not at the exact boundary:
     * skipped over, search still converges on the true boundary. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.faults[0] = (lsn_fault_t){ .lsn = 1200, .remaining = -1 };
        d.num_faults = 1;
        lsn_t got = run(&d);
        check_lsn("bad sector away from boundary is skipped", got, 1150);
    }

    /* A flaky sector right at the boundary that fails a few times before
     * succeeding: retries must recover the correct answer. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.faults[0] = (lsn_fault_t){ .lsn = 1150, .remaining = 3 };
        d.num_faults = 1;
        lsn_t got = run(&d);
        check_lsn("flaky boundary sector recovers via retries", got, 1150);
    }

    /* The exact boundary sector is permanently unreadable: the algorithm
     * must give up gracefully (CDIO_INVALID_LSN), not hang or crash. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.faults[0] = (lsn_fault_t){ .lsn = 1150, .remaining = -1 };
        d.num_faults = 1;
        lsn_t got = run(&d);
        check_lsn("permanently dead boundary sector gives up", got, CDIO_INVALID_LSN);
    }

    /* Many permanently dead sectors across the whole search window: the
     * overall failure budget must cut the search short (bounded read count)
     * rather than burning through up to 200 retries on every one of them. */
    {
        fake_disc_t d = make_disc(1000, 1150, 1300);
        d.num_faults = MAX_FAULTS;
        for (int i = 0; i < MAX_FAULTS; i++)
            d.faults[i] = (lsn_fault_t){ .lsn = 1140 + i, .remaining = -1 };
        lsn_t got = run(&d);
        check_lsn("wide dead zone gives up", got, CDIO_INVALID_LSN);
        check_true("failure budget bounds the read count", d.reads_issued < 2000);
    }

    if (fails) {
        printf("%i check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
