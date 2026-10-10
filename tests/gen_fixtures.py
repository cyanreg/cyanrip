#!/usr/bin/env python3
# Generates the CD image test fixtures in tests/fixtures/.
#
# Needs ffmpeg and a directory of audio samples (any format ffmpeg can read);
# the committed fixtures were built from private encoder test samples, but any
# three audio files >= 10 seconds long will do:
#
#   ./gen_fixtures.py /path/to/samples/dir
#
# Two BIN files are produced:
#
# cdda.bin, pure audio, 600 sectors (8 s), shared by basic.cue and pregap.cue
# (libcdio pairs cue and bin by basename, so the test runner stages copies):
#   0-225   (3 s)  audio A
#   225-450 (3 s)  audio B
#   450-600 (2 s)  audio C
#
# mixed.bin, mixed-mode CD for mixed.cue, 450 sectors (6 s), data track first:
#   0-150   (2 s)  MODE1/2352 data sectors
#   150-450 (4 s)  audio A + first second of audio B
#
# trailing.bin, mixed-mode CD for trailing.cue, 525 sectors (7 s), with the
# data track last and a pregap in front of it. A trailing data track with no
# pregap to go by is taken for a CD-Extra second session instead, and the
# 11400-sector session gap that implies will not fit in a bundled fixture:
#   0-300   (4 s)  audio A + first second of audio B
#   300-375 (1 s)  MODE1/2352 pregap sectors, no payload
#   375-525 (2 s)  MODE1/2352 data sectors
#
# The NRG reuses the first 6 s of cdda.bin audio as a 2-track DAO image with
# a 1 s pregap on track 2.

import struct
import subprocess
import sys
from pathlib import Path

SECTOR = 2352
SECTORS_PER_SEC = 75
PREGAP_SECTORS = 150  # standard 2 s lead-in pregap included in NRG images

FIXTURES = Path(__file__).parent / "fixtures"


def decode(sample, seek, seconds):
    """Decode a sample to CDDA-format PCM (s16le, 44.1 kHz, stereo)."""
    nb_bytes = seconds * SECTORS_PER_SEC * SECTOR
    p = subprocess.run(["ffmpeg", "-v", "error", "-ss", str(seek), "-i", sample,
                        "-f", "s16le", "-ar", "44100", "-ac", "2", "-"],
                       stdout=subprocess.PIPE, check=True)
    pcm = p.stdout[:nb_bytes]
    if len(pcm) < nb_bytes:
        raise SystemExit(f"{sample}: too short, wanted {seconds}s at {seek}s")
    return pcm


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def edc(data):
    """ECMA-130's EDC: CRC-32 with polynomial 0x8001801B, LSB first."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xD8018001 if crc & 1 else 0)
    return crc


def mode1_sector(lba, payload):
    """A MODE1/2352 sector: sync, BCD MSF header, mode byte, 2048 bytes of
    payload and the EDC; ECC left zeroed (nothing in the tests checks it)."""
    m, rem = divmod(lba + PREGAP_SECTORS, 60 * SECTORS_PER_SEC)
    s, f = divmod(rem, SECTORS_PER_SEC)
    sync = b"\x00" + b"\xff" * 10 + b"\x00"
    header = bytes([bcd(m), bcd(s), bcd(f), 0x01])
    data = payload[:2048].ljust(2048, b"\x00")
    sector = sync + header + data
    return (sector + struct.pack("<I", edc(sector))).ljust(SECTOR, b"\x00")


def gen_bins(audio):
    (FIXTURES / "cdda.bin").write_bytes(audio)

    data = bytearray()
    for i in range(150):
        payload = (f"CYANRIP TEST DATA sector {i:03d} " * 64).encode()
        data += mode1_sector(i, payload)
    data += audio[:300 * SECTOR]
    (FIXTURES / "mixed.bin").write_bytes(data)

    data = bytearray(audio[:300 * SECTOR])
    for i in range(300, 375):
        data += mode1_sector(i, b"")
    for i in range(375, 525):
        payload = (f"CYANRIP TEST DATA sector {i:03d} " * 64).encode()
        data += mode1_sector(i, payload)
    (FIXTURES / "trailing.bin").write_bytes(data)


def cuex_entry(track, index, lsn):
    # type (ctrl in high nibble), track, index, reserved, lsn
    track = 0xAA if track == 0xAA else bcd(track)
    return struct.pack(">BBBbi", 0x01, track, index, 0, lsn)


def chunk(cid, payload):
    return cid + struct.pack(">I", len(payload)) + payload


def gen_nrg(audio):
    """DAO audio NRG, 2 tracks of 3 s each; track 2 has a 1 s pregap starting
    at 2 s. File data area = 150 zero pregap sectors + 6 s of audio, as
    libcdio reads DAO audio at (lsn + 150) * 2352 from file start."""
    t1_start, t2_pregap, t2_start, end = 0, 150, 225, 450

    data = bytes(PREGAP_SECTORS * SECTOR) + audio[:end * SECTOR]

    # Track start/end entries come in (start, next start) pairs after the two
    # track 1 pregap/index1 entries; see libcdio lib/driver/image/nrg.c
    cuex = b"".join([
        cuex_entry(1, 0, -PREGAP_SECTORS),
        cuex_entry(1, 1, t1_start),
        cuex_entry(1, 1, t1_start), cuex_entry(2, 1, t2_start),
        cuex_entry(2, 1, t2_start), cuex_entry(0xAA, 1, end),
    ])

    def file_off(lsn):
        return (lsn + PREGAP_SECTORS) * SECTOR

    tracks = [
        (0, t1_start, t2_pregap),          # index0, index1, end_of_track (LSN)
        (t2_pregap, t2_start, end),
    ]
    daox = struct.pack("<I", 22 + 42 * len(tracks))  # chunk size again, LE
    daox += b"\x00" * 13                             # MCN
    daox += bytes([0, 0, 0])                         # unknown; [1] = disc mode
    daox += bytes([1, len(tracks)])                  # first, last track
    for i0, i1, trk_end in tracks:
        daox += b"\x00" * 12                         # ISRC
        daox += struct.pack(">HHH", SECTOR, 0x0700, 1)  # sector size, mode (0x07 = audio)
        daox += struct.pack(">QQQ", file_off(i0) if i0 else 0,
                            file_off(i1), file_off(trk_end))

    footer = chunk(b"CUEX", cuex) + chunk(b"DAOX", daox) + \
             chunk(b"SINF", struct.pack(">I", len(tracks))) + \
             chunk(b"MTYP", struct.pack(">I", 1)) + \
             chunk(b"END!", b"")

    (FIXTURES / "cdda.nrg").write_bytes(
        data + footer + b"NER5" + struct.pack(">Q", len(data)))


def main():
    samples = sorted(Path(sys.argv[1]).iterdir())
    FIXTURES.mkdir(parents=True, exist_ok=True)

    audio = decode(samples[1], 5, 3) + \
            decode(samples[3], 10, 3) + \
            decode(samples[5], 15, 2)
    gen_bins(audio)
    gen_nrg(audio)

    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi",
                    "-i", "color=red:size=8x8", "-frames:v", "1",
                    str(FIXTURES / "art.png")], check=True)
    print("fixtures written to", FIXTURES)


if __name__ == "__main__":
    main()
