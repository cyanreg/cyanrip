#!/usr/bin/env python3
"""Compare cyanrip's pregap detection against cdrdao on a real disc.

Runs `cdrdao read-toc` and `cyanrip -I` on the same drive, then lists the
start of every audio track and its pregap as each tool sees it. cdrdao
reads the Q sub-channel for the whole disc and puts the pregap length in
each track's START line; cyanrip searches around the track starts and
prints the pregap's LSN. Both are turned into an absolute LSN here.

Not a meson test: it needs a drive with an audio CD in it and takes a few
minutes, most of it cdrdao. The desktop's automounter may hold the drive;
--prepare unmounts it and stops gvfsd-cdda first.

    tests/cdrdao_compare.py                     # /dev/sr0, build/src/cyanrip
    tests/cdrdao_compare.py --toc disc.toc      # reuse an earlier cdrdao run
    tests/cdrdao_compare.py --toc disc.toc --cyanrip-output cyanrip.log
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def msf_to_frames(s):
    """cdrdao writes positions as MM:SS:FF or as a plain block count."""
    if ":" in s:
        m, sec, f = (int(x) for x in s.split(":"))
        return (m * 60 + sec) * 75 + f
    return int(s)


def run(cmd, log):
    print("+", " ".join(map(str, cmd)), flush=True)
    r = subprocess.run(list(map(str, cmd)), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    out = r.stdout.decode(errors="replace")
    log.write_text(out)
    return r.returncode, out


def prepare_drive(device):
    subprocess.run(["udisksctl", "unmount", "-b", device],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-x", "gvfsd-cdda"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def parse_toc(text):
    """Returns {track number: (start LSN, pregap frames)} for audio tracks.

    Tracks are laid out back to back from LSN 0, so a track's start is the
    sum of every segment (FILE, SILENCE, ZERO, PREGAP) before it, and the
    segments before its START line form its pregap. The 2-second lead-in
    before track 1 is not part of cdrdao's file, so it isn't counted.
    """
    tracks = {}
    number = 0
    pos = 0             # disc LSN at the end of the last segment seen
    track_begin = None  # LSN where the current track's first segment begins
    start = None        # LSN of index 1, once a START line was seen
    audio = False
    depth = 0           # inside a CD_TEXT { } block

    def close():
        if number and audio:
            index1 = track_begin if start is None else start
            tracks[number] = (index1, index1 - track_begin)

    for raw in text.splitlines():
        line = raw.split("//")[0].strip()
        if not line:
            continue
        depth += line.count("{") - line.count("}")
        if depth > 0 or line.endswith("}"):
            continue
        words = line.split()
        key = words[0]
        if key == "TRACK":
            close()
            number += 1
            track_begin, start, audio = pos, None, words[1] == "AUDIO"
        elif key in ("FILE", "AUDIOFILE", "DATAFILE"):
            pos += msf_to_frames(words[-1])
        elif key in ("SILENCE", "ZERO"):
            pos += msf_to_frames(words[1])
        elif key == "PREGAP":
            pos += msf_to_frames(words[1])
            start = pos
        elif key == "START":
            # With a length it's the pregap length, otherwise the pregap is
            # everything so far.
            start = track_begin + msf_to_frames(words[1]) if len(words) > 1 else pos
    close()
    return tracks


def parse_cyanrip(text):
    """Returns {track number: (start LSN, pregap LSN or None)}."""
    tracks = {}
    number = None
    for line in text.splitlines():
        m = re.match(r"Track (\d+) info:", line)
        if m:
            number = int(m.group(1))
            continue
        if re.match(r"Track (\d+) is data:", line):
            number = None
            continue
        if number is None:
            continue
        m = re.match(r"\s+Pregap LSN:\s+(none|-?\d+)", line)
        if m:
            tracks.setdefault(number, [None, None])[1] = \
                None if m.group(1) == "none" else int(m.group(1))
        m = re.match(r"\s+Start LSN:\s+(-?\d+)", line)
        if m:
            tracks.setdefault(number, [None, None])[0] = int(m.group(1))
    return {n: tuple(v) for n, v in tracks.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--device", default="/dev/sr0")
    ap.add_argument("--cyanrip", default=None,
                    help="cyanrip binary (default: build/src/cyanrip, else PATH)")
    ap.add_argument("--offset", type=int, default=0,
                    help="drive offset for cyanrip -s; the LSNs compared don't "
                         "depend on it")
    ap.add_argument("--toc", type=Path,
                    help="reuse this cdrdao toc file instead of reading the disc")
    ap.add_argument("--cyanrip-output", type=Path,
                    help="reuse this saved output of cyanrip -I instead of "
                         "running it")
    ap.add_argument("--workdir", type=Path, default=Path("cdrdao_compare"),
                    help="where the toc, logs and cyanrip output go")
    ap.add_argument("--prepare", action="store_true",
                    help="unmount the disc and stop gvfsd-cdda first")
    ap.add_argument("--cyanrip-arg", action="append", default=[],
                    metavar="ARG", help="extra argument for cyanrip (repeatable)")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent.parent
    cyanrip = args.cyanrip or (repo / "build" / "src" / "cyanrip")
    if not Path(cyanrip).exists():
        cyanrip = shutil.which("cyanrip")
    if not cyanrip and not args.cyanrip_output:
        sys.exit("cyanrip not found: build it or pass --cyanrip")
    if not args.toc and not shutil.which("cdrdao"):
        sys.exit("cdrdao not found")

    args.workdir.mkdir(parents=True, exist_ok=True)
    if args.prepare:
        prepare_drive(args.device)

    if args.toc:
        toc_text = args.toc.read_text()
    else:
        toc = args.workdir / "disc.toc"
        toc.unlink(missing_ok=True)
        ec, out = run(["cdrdao", "read-toc", "--device", args.device,
                       "--datafile", "disc.wav", "-v", "2", toc.resolve()],
                      args.workdir / "cdrdao.log")
        if ec != 0:
            print(out)
            sys.exit(f"cdrdao read-toc failed with {ec}")
        toc_text = toc.read_text()

    if args.cyanrip_output:
        out = args.cyanrip_output.read_text()
    else:
        ec, out = run([cyanrip, "-d", args.device, "-I", "-N", "-A", "-U",
                       "-s", args.offset, *args.cyanrip_arg],
                      args.workdir / "cyanrip.log")
        if ec != 0:
            print(out)
            sys.exit(f"cyanrip failed with {ec}")

    cdrdao_tracks = parse_toc(toc_text)
    cyanrip_tracks = parse_cyanrip(out)
    if not cdrdao_tracks or not cyanrip_tracks:
        sys.exit("could not parse the outputs "
                 f"(cdrdao: {len(cdrdao_tracks)} tracks, "
                 f"cyanrip: {len(cyanrip_tracks)})")

    for line in out.splitlines():
        if line.startswith("Q sub-channel:") or line.startswith("DiscID:"):
            print(line)

    print()
    print(f"{'track':>5} {'start':>7} {'cdrdao':>9} {'cyanrip':>9}  result")
    print(f"{'':>5} {'LSN':>7} {'pregap':>9} {'pregap':>9}")
    differences = 0
    for number in sorted(set(cdrdao_tracks) | set(cyanrip_tracks)):
        a = cdrdao_tracks.get(number)
        b = cyanrip_tracks.get(number)
        if a is None or b is None:
            who = "cdrdao" if a is None else "cyanrip"
            print(f"{number:>5} {'':>7} {'':>9} {'':>9}  not an audio track for {who}")
            continue
        start_a, pregap_a = a
        start_b, pregap_lsn = b
        # Both in frames before index 1; cyanrip says "none" for no pregap
        pregap_b = 0 if pregap_lsn is None else start_b - pregap_lsn
        notes = []
        if start_a != start_b:
            notes.append(f"cdrdao start {start_a}")
        if pregap_a != pregap_b:
            notes.append("DIFFERENT")
            differences += 1
        print(f"{number:>5} {start_b:>7} {pregap_a:>9} {pregap_b:>9}  {' '.join(notes) or 'same'}")

    print()
    if differences:
        print(f"{differences} track(s) differ; outputs are in {args.workdir}/")
        sys.exit(1)
    print("cdrdao and cyanrip agree on all pregaps")


if __name__ == "__main__":
    main()
