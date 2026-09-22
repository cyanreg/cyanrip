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
 * drive - no real libcdio driver or hardware involved, and this test binary
 * does not even link against libcdio.so: pregap.c calls cdio_get_track_lsn(),
 * cdio_get_first_track_num(), cdio_get_track_format(),
 * cdio_get_track_pregap_lsn(), and cyanrip_read_audio_subq_sector() by name,
 * and every one of them is defined right here instead, describing a synthetic
 * disc (`disc`) instead of talking to a real drive. Since none of the real
 * implementations are linked in (see tests/meson.build), there's no symbol
 * clash - the linker just resolves pregap.c's calls to these definitions.
 *
 * This lets us inject drive misbehaviour (spurious reads, permanently bad
 * sectors, BCD-quirk drives) that would be impractical to reproduce with
 * real hardware or a bin/cue image.
 *
 * cyanrip_get_track_pregap_lsn() is still called with a real cyanrip_ctx
 * (zeroed, like naming_test.c does): that's where ctx->subq_needs_bcd_fixup
 * lives. cyanrip_ctx has no room for a synthetic track layout or fault
 * injection though, and shouldn't grow any just for this test, so the
 * overridden functions below read the disc being ripped from a separate
 * test-only fixture (`disc`) instead of from ctx.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "pregap.h"
#include "cyanrip_main.h"
#include "cyanrip_log.h"
#include "subq_read.h"

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
    lsn_t stale; /* the drive can't decode this sector's Q: the formatted read hands
                  * back the frame before it, raw P-W the damaged frame itself */
    int stale_damage; /* what the damage hit: 0 the spare byte only, 1 one bit of
                       * the absolute time, 2 both, i.e. beyond a single bit repair */
    int q_offset; /* Q sub-channel runs this many sectors ahead of the TOC */
    lsn_t ctx_start_lsn; /* fed to cyanrip_ctx.start_lsn in run(); only matters for the first track */

    lsn_fault_t faults[MAX_FAULTS];
    int num_faults;
    lsn_jitter_t jitter[MAX_JITTER];
    int num_jitter;
    lsn_t mode2[MAX_MODE2]; /* sectors whose Q frame is mode 2 (catalogue number): no position data */
    int num_mode2;

    int reads_issued;
    enum cyanrip_subq_read_mode mode_chosen;
} fake_disc_t;

/* The disc/drive the overridden cdio_get_* and cyanrip_read_audio_subq_sector()
 * functions below serve. Reset by make_disc() at the start of each scenario;
 * only one scenario is ever in flight at a time. */
static fake_disc_t disc;

static void make_disc(lsn_t prev_start, lsn_t pregap_start, lsn_t cur_start)
{
    memset(&disc, 0, sizeof(disc));
    disc.stale = CDIO_INVALID_LSN;
    disc.first_track_num = 1;
    disc.prev_track_number = 5;
    disc.cur_track_number = 6;
    disc.prev_track_start_lsn = prev_start;
    disc.cur_pregap_start_lsn = pregap_start;
    disc.cur_track_start_lsn = cur_start;
    disc.prev_track_format = TRACK_FORMAT_AUDIO;
    disc.cur_track_format = TRACK_FORMAT_AUDIO;
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

/* Substitutes for the real cyanrip_read_audio_subq_sector() (normally
 * implemented in subq_read_mmc.c/subq_read_macos.c, neither linked into this
 * test binary): generates synthetic Q sub-channel bytes for `disc` instead
 * of talking to real hardware. */
/* Builds the 16 byte Q sub-channel frame of a sector as the drive would hand
 * it back formatted: 12 bytes of Q, zero padded. Returns 0 when the sector is
 * faulted and the frame is left all zero (CRC field 0, never valid). */
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
            /* adr=2: the fields below stand in for the catalogue number
             * digits, all pregap.c may look at is the adr and the CRC. */
            q[0] = (0x1 << 4) | 0x2;
            break;
        }
    }
    q[1] = bin_to_bcd(true_track);
    q[2] = bin_to_bcd(true_index);
    q[3] = bin_to_bcd(0);
    q[4] = bin_to_bcd(12); /* relative seconds: >=10 so BCD vs binary actually differ */
    q[5] = bin_to_bcd(0);
    q[6] = 0;
    /* absolute time: the sector the Q frame really belongs to, 2s lead-in included */
    const lsn_t abs_frames = content_lsn + CDIO_PREGAP_SECTORS;
    q[7] = bin_to_bcd((uint8_t)(abs_frames / (60 * 75)));
    q[8] = bin_to_bcd((uint8_t)(abs_frames / 75 % 60));
    q[9] = bin_to_bcd((uint8_t)(abs_frames % 75));

    unsigned crc = test_crc_subq(q);
    q[10] = (crc >> 8) & 0xFF;
    q[11] = crc & 0xFF;
    return 1;
}

/* Substitutes for the real cyanrip_read_audio_subq_sector() (normally
 * implemented in subq_read_mmc.c/subq_read_macos.c, neither linked into this
 * test binary): generates synthetic formatted Q sub-channel bytes for `disc`
 * instead of talking to real hardware, with the drive quirks the disc asks
 * for layered on top. */
driver_return_code_t cyanrip_read_audio_subq_sector(const CdIo_t *p_cdio, uint8_t *buf,
                                                      lsn_t lsn)
{
    (void)p_cdio;
    disc.reads_issued++;

    uint8_t *q = buf + CDIO_CD_FRAMESIZE_RAW;

    /* The drive couldn't decode this sector's Q and hands back the last
     * frame it did decode, CRC and all. */
    if (lsn == disc.stale)
        lsn -= 1;

    if (!fake_subq_frame(lsn, q))
        return DRIVER_OP_SUCCESS;

    if (disc.nonbcd) {
        /* Simulate a drive whose firmware hands back raw binary values
         * instead of BCD for these fields (the CRC above was computed over
         * the correct BCD bytes first). */
        static const int fields[] = { 1, 2, 3, 4, 5, 7, 8, 9 };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
            q[fields[i]] = bcd_to_bin(q[fields[i]]);
    }
    if (disc.nocrc)
        q[10] = q[11] = 0;
    return DRIVER_OP_SUCCESS;
}

/* Raw P-W: the same frames, spread one bit per subcode symbol into bit 6,
 * with P (bit 7) set throughout. No drive quirks apply, since the drive
 * passes these bits through untouched; only what is on the disc matters. */
driver_return_code_t cyanrip_read_audio_subpw_sector(const CdIo_t *p_cdio, uint8_t *buf,
                                                       lsn_t lsn)
{
    (void)p_cdio;

    if (disc.no_raw_pw)
        return DRIVER_OP_UNSUPPORTED;

    disc.reads_issued++;

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

/* Overrides for the real libcdio track-metadata queries: this test binary
 * doesn't link libcdio.so at all (see tests/meson.build), so these are the
 * only definitions the linker ever sees for these symbols. */

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
    memset(&ctx, 0, sizeof(ctx)); /* fresh ctx each time: subq_needs_bcd_fixup starts at 0 */
    ctx.start_lsn = disc.ctx_start_lsn;
    disc.reads_issued = 0;
    lsn_t got = cyanrip_get_track_pregap_lsn(&ctx, disc.cur_track_number);
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
    /* First track: nothing precedes it to hold a pregap, so there is no
     * boundary to search and no subq work to do - its pregap is always the
     * disc's start, so that the lead-in is reported, even when the track
     * begins there and the pregap is therefore empty. */
    {
        make_disc(1000, 1150, 1300);
        disc.cur_track_number = disc.first_track_num;
        disc.ctx_start_lsn = disc.cur_track_start_lsn; /* no lead-in gap */
        lsn_t got = run();
        check_lsn("first track, no lead-in gap", got, disc.ctx_start_lsn);
        check_true("first track, no lead-in gap: no subq reads", disc.reads_issued == 0);
    }

    /* First track with a lead-in gap (e.g. a hidden track before it): the
     * disc's start is the pregap, still without any subq work. */
    {
        make_disc(1000, 1150, 1300);
        disc.cur_track_number = disc.first_track_num;
        disc.ctx_start_lsn = 0;
        lsn_t got = run();
        check_lsn("first track, lead-in gap", got, disc.ctx_start_lsn);
        check_true("first track, lead-in gap: no subq reads", disc.reads_issued == 0);
    }

    /* libcdio already knows the pregap (e.g. a cue sheet): use it directly,
     * no subq reads needed at all. */
    {
        make_disc(1000, 1150, 1300);
        disc.simulate_libcdio_pregap_support = 1;
        lsn_t got = run();
        check_lsn("libcdio-reported pregap", got, 1150);
        check_true("libcdio-reported pregap: no subq reads", disc.reads_issued == 0);
    }

    /* No pregap at all: fast path should confirm with just two reads, and
     * report the absence as CDIO_INVALID_LSN rather than as a zero length
     * pregap sitting on the track start. */
    {
        make_disc(1000, 1300, 1300);
        lsn_t got = run();
        check_lsn("no pregap", got, CDIO_INVALID_LSN);
        check_true("no pregap: fast path used only 2 reads", disc.reads_issued == SUBQ_PROBE_SECTORS + 2);
    }

    /* Previous track is a single sector: no room for a pregap, reported as
     * CDIO_INVALID_LSN like any other absent pregap, without any subq work. */
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

    /* Long pregap spanning several 150-sector backtrack jumps. */
    {
        make_disc(1000, 1500, 2000);
        lsn_t got = run();
        check_lsn("long pregap", got, 1500);
    }

    /* Data track adjacent to the boundary: must bail out immediately
     * without doing any subq work at all. */
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

    /* Which way the Q sub-channel gets read: raw P-W when the drive can, the
     * formatted Q when it can't or what it returns raw is junk. Reading the
     * formatted Q through a drive that could do raw would be a regression. */
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

    /* A sector the drive can't decode the Q of, right at the boundary. The
     * formatted Q hands back the previous sector's frame with a valid CRC,
     * which nothing can tell from a real read: the pregap comes out one
     * sector short. Raw P-W exposes the failed CRC instead, and the frame's
     * surviving payload still places the boundary. */
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

    /* A single bit error in the payload is pinned down by the CRC and
     * repaired; two bit errors are beyond that and leave nothing to go by. */
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1150;
        disc.stale_damage = 1;
        lsn_t got = run();
        check_lsn("damaged frame with a single bit error is repaired", got, 1150);
    }
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1150;
        disc.stale_damage = 2;
        lsn_t got = run();
        check_lsn("damaged frame with two bit errors gives up", got, CDIO_INVALID_LSN);
    }

    /* The same stale sector away from the boundary is harmless either way. */
    {
        make_disc(1000, 1150, 1300);
        disc.stale = 1200;
        lsn_t got = run();
        check_lsn("stale frame inside the pregap", got, 1150);
    }

    /* A drive whose firmware returns raw binary MSF fields instead of BCD
     * must still resolve the pregap correctly once the quirk is detected. */
    {
        make_disc(1000, 1150, 1300);
        disc.no_raw_pw = 1;
        disc.nonbcd = 1;
        lsn_t got = run();
        check_lsn("non-BCD drive quirk", got, 1150);
    }

    /* A drive that doesn't supply the CRC with the formatted Q: frames are
     * vetted by their absolute time instead, in whichever encoding fits. */
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

    /* On such a drive a read of the wrong sector gives itself away by its
     * absolute time and never becomes a candidate at all. */
    {
        make_disc(1000, 1300, 1500);
        disc.no_raw_pw = 1;
        disc.nocrc = 1;
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1250, .reports_as = 1305 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("drive without Q CRC: wrong sector read is rejected", got, 1300);
    }

    /* The two together: no CRC, and Q handed back 2 sectors early. */
    {
        make_disc(1000, 1150, 1300);
        disc.no_raw_pw = 1;
        disc.nocrc = 1;
        disc.q_offset = 2;
        lsn_t got = run();
        check_lsn("drive without Q CRC, Q ahead of the TOC", got, 1150);
    }

    /* A single spuriously CRC-valid read for the wrong physical sector (seek
     * jitter near the boundary) must not be trusted on its own: the sector
     * right after it will contradict it and the search must still land on
     * the true boundary, not the jittered one. */
    {
        make_disc(1000, 1300, 1500);
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1250, .reports_as = 1305 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("single spurious read is not trusted", got, 1300);
    }

    /* A spurious read reporting the *previous* track (the mirror image of the
     * case above) must not be trusted either: taken at face value during
     * backtracking it would anchor the left bound inside the pregap and the
     * search would silently converge on a wrong, too-late boundary. */
    {
        make_disc(1000, 1100, 2000);
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1249, .reports_as = 1050 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("single spurious prev-track read is not trusted", got, 1100);
    }

    /* A sector that's permanently unreadable but not at the exact boundary:
     * skipped over, search still converges on the true boundary. This one sits
     * both on a backtrack landing spot and inside the range the shrinking loop
     * scans, so both have to tolerate it. */
    {
        make_disc(1000, 1200, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1149, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("bad sector away from boundary is skipped", got, 1200);
    }

    /* A pregap exactly one sector long: the sector that would normally confirm
     * the boundary is the track start itself, which is already known to belong
     * to the new track and must count as the confirmation. */
    {
        make_disc(1000, 1299, 1300);
        lsn_t got = run();
        check_lsn("one sector pregap", got, 1299);
    }

    /* No pregap, but the Q sub-channel runs ahead of the TOC, so the sectors
     * just below the track start already report the new track. They carry
     * index 1, not index 0: that is the track itself, not a pregap. */
    {
        make_disc(1000, 1300, 1300);
        disc.q_offset = 2;
        lsn_t got = run();
        check_lsn("no pregap, Q ahead of the TOC", got, CDIO_INVALID_LSN);
    }

    /* Same skew with a real pregap: the index 0 sectors turn up 2 sectors
     * early, but their absolute time says where they really are. */
    {
        make_disc(1000, 1150, 1300);
        disc.q_offset = 2;
        lsn_t got = run();
        check_lsn("pregap, Q ahead of the TOC", got, 1150);
    }

    /* And with the Q sub-channel running behind instead. */
    {
        make_disc(1000, 1150, 1300);
        disc.q_offset = -2;
        lsn_t got = run();
        check_lsn("pregap, Q behind the TOC", got, 1150);
    }

    /* The frames on the two bounds don't claim to be neighbours, so the drive
     * wasn't off by the same amount for both: the absolute time can't be
     * trusted to place the boundary and the sector asked for is kept. */
    {
        make_disc(1000, 1150, 1300);
        disc.jitter[0] = (lsn_jitter_t){ .lsn = 1150, .reports_as = 1153 };
        disc.num_jitter = 1;
        lsn_t got = run();
        check_lsn("unsteady Q skew at the boundary", got, 1150);
    }

    /* A mode 2 Q frame (catalogue number) on the second sector of the pregap:
     * it can't confirm the first one, but it doesn't contradict it either, so
     * the next sector reporting the new track still does. */
    {
        make_disc(1000, 1150, 1300);
        disc.mode2[0] = 1151;
        disc.num_mode2 = 1;
        lsn_t got = run();
        check_lsn("mode 2 Q frame right after the boundary", got, 1150);
    }

    /* Same with a permanently unreadable sector there, and with both kinds
     * back to back. */
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

    /* A two sector pregap whose second sector is dead: the track start is an
     * established new-track sector and confirms the first one across it. */
    {
        make_disc(1000, 1298, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1299, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("two sector pregap, second one dead", got, 1298);
    }

    /* Tolerating silent sectors after a candidate must not let a spurious
     * read through: the previous-track sectors that follow still reject it. */
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

    /* A dead sector in the middle of a long scanned range: the shrinking loop
     * has to step over it and rule it out by moving the left bound past it. */
    {
        make_disc(1000, 1500, 2000);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1450, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("bad sector inside the scanned range is skipped", got, 1500);
    }

    /* A flaky sector right at the boundary that fails a few times before
     * succeeding: retries must recover the correct answer. */
    {
        make_disc(1000, 1150, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1150, .remaining = 3 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("flaky boundary sector recovers via retries", got, 1150);
    }

    /* The exact boundary sector is permanently unreadable: the algorithm
     * must give up gracefully (CDIO_INVALID_LSN), not hang or crash. */
    {
        make_disc(1000, 1150, 1300);
        disc.faults[0] = (lsn_fault_t){ .lsn = 1150, .remaining = -1 };
        disc.num_faults = 1;
        lsn_t got = run();
        check_lsn("permanently dead boundary sector gives up", got, CDIO_INVALID_LSN);
    }

    /* Many permanently dead sectors across the whole search window: the
     * overall failure budget must cut the search short (bounded read count)
     * rather than burning through up to 200 retries on every one of them. */
    {
        make_disc(1000, 1150, 1300);
        disc.num_faults = MAX_FAULTS;
        for (int i = 0; i < MAX_FAULTS; i++)
            disc.faults[i] = (lsn_fault_t){ .lsn = 1140 + i, .remaining = -1 };
        lsn_t got = run();
        check_lsn("wide dead zone gives up", got, CDIO_INVALID_LSN);
        check_true("failure budget bounds the read count", disc.reads_issued < 2000);
    }

    if (fails) {
        printf("%i check(s) failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
