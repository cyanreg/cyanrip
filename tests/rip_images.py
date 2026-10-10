#!/usr/bin/env python3
# Rips the disc image fixtures and verifies the finished files.
# Usage: rip_images.py <cyanrip-binary> <fixtures-dir> <scenario>

import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CRIP = sys.argv[1]
FIX = Path(sys.argv[2])
SCENARIO = sys.argv[3]

FFPROBE = shutil.which("ffprobe")

fails = 0


def fail(msg):
    global fails
    print("FAIL:", msg)
    fails += 1


def crip(*args):
    r = subprocess.run([CRIP, *map(str, args)], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=60)
    return r.returncode, r.stdout.decode(errors="replace")


def rip(name, img, *extra):
    ec, log = crip("-d", WORK / img, "-N", "-A", "-U", "-s", "0", "-P", "0",
                   "-o", "flac", "-D", WORK / f"out_{name}", "-F", "{track}",
                   "-L", "log", "-M", "sheet", *extra)
    (WORK / f"{name}.log").write_text(log)
    if ec != 0:
        fail(f"{name}: cyanrip exited with {ec} (log follows)")
        print(log)


def probe(path, *entries):
    r = subprocess.run([FFPROBE, "-v", "error", *entries, "-of",
                        "default=nw=1:nk=1", str(path)],
                       stdout=subprocess.PIPE, timeout=60)
    return r.stdout.decode().strip()


def pcm_md5(name, track):
    data = (WORK / f"out_{name}" / f"{track}.pcm").read_bytes()
    return hashlib.md5(data).hexdigest()


def expect(name, *specs):
    out = WORK / f"out_{name}"
    want = sorted(s.split(":")[0] for s in specs)
    have = sorted(p.name for p in out.iterdir()) if out.is_dir() else []
    if have != want:
        fail(f"{name}: outputs {have} != expected {want}")
        return

    for spec in specs:
        f, _, dur = spec.partition(":")
        path = out / f

        if f.endswith(".flac") and path.read_bytes()[:4] != b"fLaC":
            fail(f"{name}: {f} is not FLAC")

        if dur and FFPROBE:
            d = probe(path, "-show_entries", "format=duration")
            try:
                ok = abs(float(d) - float(dur)) <= 0.1
            except ValueError:
                ok = False
            if not ok:
                fail(f"{name}: {f} duration {d} != {dur}")


def sc_info():
    # Info-only mode on every image type
    for img in ("basic.cue", "pregap.cue", "mixed.cue", "preemph.cue",
                "cdda.nrg"):
        ec, _ = crip("-d", WORK / img, "-I", "-N", "-A", "-U", "-P", "0")
        if ec != 0:
            fail(f"info {img}: cyanrip exited with {ec}")


def sc_basic():
    rip("basic", "basic.cue")
    expect("basic", "1.flac:4", "2.flac:4", "log.log", "sheet.cue")


def log_frames(name):
    """Frame counts the log reports, one per track"""
    log = (WORK / f"out_{name}" / "log.log").read_text()
    return [int(l.split()[1]) for l in log.splitlines()
            if l.strip().startswith("Frames:")]


def sc_pregap():
    # Track 1 HTOA stays unmerged by default, track 2 pregap merges into track 1
    rip("def", "pregap.cue")
    expect("def", "1.flac:3", "2.flac:2", "3.flac:1", "log.log", "sheet.cue")

    # HTOA becomes track 0, track 2 pregap becomes a track of its own
    rip("track", "pregap.cue", "-p", "1=track", "-p", "2=track")
    expect("track", "0.flac:2", "1.flac:2", "2.flac:1", "3.flac:2",
           "4.flac:1", "log.log", "sheet.cue")

    # The log describes the files, split off tracks and the ones they were
    # split from included
    if log_frames("track") != [150, 150, 75, 150, 75]:
        fail(f"track: logged frames {log_frames('track')}")

    rip("drop", "pregap.cue", "-p", "2=drop")
    expect("drop", "1.flac:2", "2.flac:2", "3.flac:1", "log.log", "sheet.cue")

    # A merged pregap starts the file, so index 1 comes a second in
    rip("merge", "pregap.cue", "-p", "2=merge")
    expect("merge", "1.flac:2", "2.flac:3", "3.flac:1", "log.log", "sheet.cue")
    if log_frames("merge") != [150, 225, 75]:
        fail(f"merge: logged frames {log_frames('merge')}")

    sheet = (WORK / "out_merge" / "sheet.cue").read_text()
    if "INDEX 00 00:00:00\n    INDEX 01 00:01:00" not in sheet:
        fail("merge: track 2 indices don't match its merged pregap")


def data_track(name, img, track, lsn, frames):
    """The ripped data track must be the image's sectors, byte for byte"""
    want = (WORK / img).read_bytes()[lsn * 2352:(lsn + frames) * 2352]
    if (WORK / f"out_{name}" / f"{track}.bin").read_bytes() != want:
        fail(f"{name}: {track}.bin does not match sectors {lsn}-{lsn + frames}")


def sc_mixed():
    # The data track rips to a raw MODE1/2352 binary
    rip("mixed", "mixed.cue")
    expect("mixed", "1.bin", "2.flac:2", "3.flac:2", "log.log", "sheet.cue")
    data_track("mixed", "mixed.bin", 1, 0, 150)

    sheet = (WORK / "out_mixed" / "sheet.cue").read_text()
    if 'FILE "1.bin" BINARY' not in sheet or "TRACK 01 MODE1/2352" not in sheet:
        fail("mixed: the CUE sheet does not reference the data track")

    # Data track selected via rip indices
    rip("idx", "mixed.cue", "-l", "1,2")
    expect("idx", "1.bin", "2.flac:2", "log.log", "sheet.cue")

    # -X leaves data tracks alone, and produces no stray file
    rip("nodata", "mixed.cue", "-X")
    expect("nodata", "2.flac:2", "3.flac:2", "log.log", "sheet.cue")

    # Every sector's EDC is checked, so a damaged one is caught even when the
    # read itself succeeds
    if "Sector EDC:    all 150 sectors match" not in (WORK / "mixed.log").read_text():
        fail("mixed: data track EDCs not verified")

    shutil.copy(WORK / "mixed.cue", WORK / "damaged.cue")
    damaged = bytearray((WORK / "mixed.bin").read_bytes())
    damaged[10 * 2352 + 100] ^= 0x01
    (WORK / "damaged.bin").write_bytes(damaged)
    ec, log = crip("-d", WORK / "damaged.cue", "-N", "-A", "-U", "-s", "0",
                   "-P", "0", "-r", "1", "-o", "flac", "-D", WORK / "out_damaged",
                   "-L", "log", "-M", "sheet")
    if ec == 0 or "Sector EDC mismatch at LSN 10!" not in log or \
       "1 of 150 sectors mismatched" not in log:
        fail(f"damaged: EDC mismatch not reported (exit {ec})")


def sc_trailing():
    # A trailing data track with a pregap: the pregap is dropped rather than
    # taken for a CD-Extra session gap, so no audio is lost to it
    rip("trailing", "trailing.cue")
    expect("trailing", "1.flac:2", "2.flac:2", "3.bin", "log.log", "sheet.cue")
    data_track("trailing", "trailing.bin", 3, 375, 150)

    # Merging the pregap prepends it to the data track instead
    rip("merge", "trailing.cue", "-p", "3=merge")
    expect("merge", "1.flac:2", "2.flac:2", "3.bin", "log.log", "sheet.cue")
    data_track("merge", "trailing.bin", 3, 300, 225)


def write_toc(name, sectors, tracks):
    """A CUE sheet and a matching (sparse) BIN, tracks are number/type/LSN"""
    with open(WORK / f"{name}.bin", "wb") as f:
        f.truncate(sectors * 2352)

    lines = [f'FILE "{name}.bin" BINARY']
    for number, kind, lsn in tracks:
        lines.append(f"  TRACK {number:02d} {kind}")
        lines.append("    INDEX 01 %02d:%02d:%02d"
                     % (lsn // (75 * 60), lsn // 75 % 60, lsn % 75))
    (WORK / f"{name}.cue").write_text("\n".join(lines) + "\n")


def check_ids(img, discid, cddb, toc, arid):
    ec, log = crip("-d", WORK / img, "-I", "-N", "-A", "-U", "-P", "0")
    if ec != 0:
        fail(f"discid {img}: cyanrip exited with {ec}")
        return

    have = {}
    for line in log.splitlines():
        line = line.strip()
        if line.startswith("DiscID:"):
            have["discid"] = line.split()[-1]
        elif line.startswith("CDDB ID:"):
            have["cddb"] = line.split()[-1]
        elif line.startswith("AccurateRip ID:"):
            have["arid"] = line.split()[-1]
        elif "attach?toc=" in line:
            have["toc"] = line.split("attach?toc=")[1].split("&")[0]

    for key, want in (("discid", discid), ("cddb", cddb), ("toc", toc),
                      ("arid", arid)):
        if have.get(key) != want:
            fail(f"discid {img}: {key} {have.get(key)} != {want}")


def sc_discid():
    # The DiscID, CDDB ID and TOC are what libdiscid's mb_disc_load_toc() and
    # create_disc_id() produce for the same TOC. The AccurateRip ID weights
    # audio tracks by list position, uses the real lead-out, and carries a
    # FreeDB ID computed over the full TOC, data tracks included; the whole
    # combination was verified against live AccurateRip database entries.

    # A data track at the front is still a part of the hashed TOC
    check_ids("mixed.cue", "IWFVjcDW_sHbcouxZ99EXu2rOyw-", "0C000603",
              "1+3+600+150+300+450", "00000384-00000834-0c000603")

    # A trailing data track stands in for the lead-out, one session gap
    # early, in the MusicBrainz IDs alone - AccurateRip keeps the real one
    write_toc("extra", 14400, [(1, "AUDIO", 0), (2, "AUDIO", 1000),
                               (3, "MODE1/2352", 13400)])
    check_ids("extra.cue", "1R2GmWmzvVJpk3OOqrZc8TEHcmQ-", "08001A02",
              "1+2+2150+150+1150", "00003c28-0000b091-1100c003")

    # Only the first of several trailing data tracks decides the lead-out;
    # the extra data track still counts towards AccurateRip's FreeDB ID
    write_toc("extra2", 14400, [(1, "AUDIO", 0), (2, "AUDIO", 1000),
                                (3, "MODE1/2352", 13400),
                                (4, "MODE1/2352", 14000)])
    check_ids("extra2.cue", "1R2GmWmzvVJpk3OOqrZc8TEHcmQ-", "08001A02",
              "1+2+2150+150+1150", "00003c28-0000b091-2200c004")

    # Offsets are hashed by track number rather than by position
    write_toc("first3", 3000, [(3, "AUDIO", 0), (4, "AUDIO", 1000),
                               (5, "AUDIO", 2000)])
    check_ids("first3.cue", "Hq2wagZiFOC__Il_nRSlGDfLpzc-", "12002A05",
              "3+5+3150+150+1150+2150", "00001770-00004e21-12002803")


def sc_nrg():
    # NRG with a DAOX pregap on track 2
    rip("nrg", "cdda.nrg")
    expect("nrg", "1.flac:3", "2.flac:3", "log.log", "sheet.cue")


def sc_filters():
    # The HDCD and deemphasis filter graphs, verified on raw PCM output
    rip("plain", "basic.cue", "-o", "pcm")
    expect("plain", "1.pcm", "2.pcm", "log.log", "sheet.cue")
    plain_size = (WORK / "out_plain" / "1.pcm").stat().st_size
    if plain_size != 4 * 44100 * 2 * 2:
        fail(f"plain: 1.pcm is {plain_size} bytes")

    # HDCD decodes to 24-bit, so raw output must be s32
    rip("hdcd", "basic.cue", "-o", "pcm", "-H")
    hdcd_size = (WORK / "out_hdcd" / "1.pcm").stat().st_size
    if hdcd_size != 2 * plain_size:
        fail(f"hdcd: 1.pcm is {hdcd_size} bytes, wanted {2 * plain_size}")

    # Forced deemphasis must actually alter the audio
    rip("forced", "basic.cue", "-o", "pcm", "-E")
    if pcm_md5("forced", 1) == pcm_md5("plain", 1):
        fail("-E did not change the audio")

    # TOC preemphasis flags trigger automatic deemphasis, matching -E;
    # -W disables it, matching an unfiltered rip
    rip("auto", "preemph.cue", "-o", "pcm")
    if pcm_md5("auto", 1) != pcm_md5("forced", 1):
        fail("automatic deemphasis output doesn't match -E")

    rip("off", "preemph.cue", "-o", "pcm", "-W")
    if pcm_md5("off", 1) != pcm_md5("plain", 1):
        fail("-W did not disable deemphasis")


def sc_art():
    # Album cover art: written out per format and embedded in every track
    rip("art", "basic.cue", "-C", f"Front={FIX / 'art.png'}")
    expect("art", "1.flac:4", "2.flac:4", "Front.png", "log.log", "sheet.cue")
    if FFPROBE:
        for f in (1, 2):
            pics = probe(WORK / "out_art" / f"{f}.flac", "-select_streams",
                         "v", "-show_entries", "stream=codec_name")
            if len(pics.splitlines()) != 1:
                fail(f"art: {f}.flac embedded pictures: {pics!r}, wanted 1")

            # Typed as a front cover, or file managers won't thumbnail it
            ptype = probe(WORK / "out_art" / f"{f}.flac", "-select_streams",
                          "v", "-show_entries", "stream_tags=comment")
            if ptype != "Cover (front)":
                fail(f"art: {f}.flac picture type {ptype!r}")


def fingerprint(name, track):
    return probe(WORK / f"out_{name}" / f"{track}.flac", "-show_entries",
                 "format_tags=acoustid_fingerprint")


def sc_acoustid():
    # Nothing can be verified without the AccurateRip and MusicBrainz
    # lookups, so the fingerprints go in the tags rather than to AcoustID
    rip("fp", "basic.cue")
    log = (WORK / "fp.log").read_text()
    if "built without libchromaprint" in log:
        print("acoustid: built without libchromaprint, skipping")
        return
    if "2 fingerprints saved to tags, not submitted: AccurateRip disabled" not in log:
        fail("fp: fingerprints not reported as saved to tags")
    if not FFPROBE:
        return

    fps = [fingerprint("fp", t) for t in (1, 2)]
    if not all(fp.startswith("AQ") for fp in fps) or fps[0] == fps[1]:
        fail(f"fp: fingerprint tags {fps!r}")

    # Fingerprints are kilobytes long on real tracks, they stay out of the log
    if fps[0] and fps[0] in (WORK / "out_fp" / "log.log").read_text():
        fail("fp: fingerprint written to the rip log")

    # The files must wait for the fingerprints even without ReplayGain
    rip("fp_norg", "basic.cue", "-K")
    if fingerprint("fp_norg", 1) != fps[0]:
        fail("fp_norg: no fingerprint tag without ReplayGain")

    rip("fp_off", "basic.cue", "-B")
    if fingerprint("fp_off", 1):
        fail("fp_off: fingerprint tagged despite -B")


def sc_cue_only():
    # -J generates and prints the CUE sheet without ripping anything
    rip("cue", "pregap.cue", "-J")
    have = sorted(p.name for p in (WORK / "out_cue").iterdir())
    if have != ["sheet.cue"]:
        fail(f"cue_only: outputs {have}, wanted only the CUE sheet")

    cue = (WORK / "out_cue" / "sheet.cue").read_text()
    if 'FILE "1.flac" WAVE' not in cue:
        fail("cue_only: file references are not relative to the sheet")
    if "TRACK 03 AUDIO" not in cue:
        fail("cue_only: cue sheet incomplete")

    if "TRACK 01 AUDIO" not in (WORK / "cue.log").read_text():
        fail("cue_only: cue sheet not printed to the terminal")

    if crip("-d", WORK / "basic.cue", "-J", "-I")[0] != 1:
        fail("cue_only: -J with -I did not error out")


def sc_errors():
    # Schemes sending multiple tracks to one file must be warned about
    rip("collide", "basic.cue", "-F", "{album}")
    if "resolve to the same file" not in (WORK / "collide.log").read_text():
        fail("collide: expected a filename collision warning")

    # Encoder init failure (file name too long) must fail cleanly, not
    # crash in cleanup on the uninitialized encoder mutex/thread
    ec, _ = crip("-d", WORK / "basic.cue", "-N", "-A", "-U", "-s", "0",
                 "-P", "0", "-K", "-o", "flac", "-D", WORK / "out_longname",
                 "-F", "x" * 300, "-L", "log", "-M", "sheet")
    if ec != 1:
        fail(f"longname: expected clean failure (1), got exit {ec}")


def sc_verify_log():
    # CLI wiring only, the checksum logic itself is unit-tested
    rip("basic", "basic.cue")
    log = WORK / "out_basic" / "log.log"
    if crip("--verify-log", log)[0] != 0:
        fail("valid log did not verify")

    tampered = WORK / "tampered.log"
    tampered.write_text(log.read_text().replace("Ripping errors: 0",
                                                "Ripping errors: 1"))
    if crip("--verify-log", tampered)[0] == 0:
        fail("tampered log verified")


with tempfile.TemporaryDirectory() as tmpdir:
    WORK = Path(tmpdir)

    # libcdio pairs .cue and .bin files by basename, so stage a copy per sheet
    for f in FIX.glob("*.cue"):
        shutil.copy(f, WORK)
    shutil.copy(FIX / "cdda.nrg", WORK)
    for name in ("basic", "pregap", "preemph"):
        shutil.copy(FIX / "cdda.bin", WORK / f"{name}.bin")
    for name in ("mixed", "trailing"):
        shutil.copy(FIX / f"{name}.bin", WORK / f"{name}.bin")

    globals()[f"sc_{SCENARIO}"]()

if fails:
    print(f"{fails} check(s) failed")
    sys.exit(1)
print(SCENARIO, "passed")
