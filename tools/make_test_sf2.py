#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Writes a tiny SoundFont (SF2) of sine samples and instrument packs around it, for the instrument tests (tests/instrument_test.cpp).

The soundfont is our own work (a computed sine, nothing recorded or copied): one preset "Test Sine" (bank 0, program 0) with one looped sample of
a 440 Hz sine on key 69 (A4). The sample is stored at 44000 Hz, so that one period is exactly 100 samples and the loop is seamless; FluidSynth
resamples it, and a note N sounds at 440 * 2**((N - 69) / 12) Hz.

    make_test_sf2.py OUTDIR            writes OUTDIR/rovox-test-sine/{pack.json,sine.sf2}  (licence AGPL-3.0-or-later, server_use_ok true)
                                       and OUTDIR/rovox-test-unknown/{pack.json,sine.sf2}  (licence unknown, server_use_ok "unknown")
    make_test_sf2.py --sf2 FILE        writes only the soundfont

The output is byte-for-byte the same on every run.
"""

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

SAMPLE_RATE = 44000
PERIOD = 100            # samples per period of the 440 Hz sine at SAMPLE_RATE
PERIODS = 20
LENGTH = PERIOD * PERIODS
LOOP_START = PERIOD
LOOP_END = LENGTH - PERIOD
ROOT_KEY = 69
AMPLITUDE = 0.5


def chunk(tag: bytes, body: bytes) -> bytes:
    if len(body) % 2:
        body += b"\0"
    return tag + struct.pack("<I", len(body)) + body


def riff_list(kind: bytes, *chunks: bytes) -> bytes:
    return chunk(b"LIST", kind + b"".join(chunks))


def name20(text: str) -> bytes:
    return text.encode("ascii")[:19].ljust(20, b"\0")


def build_sf2() -> bytes:
    samples = [round(AMPLITUDE * 32767 * math.sin(2 * math.pi * i / PERIOD)) for i in range(LENGTH)]
    smpl = struct.pack("<%dh" % LENGTH, *samples) + b"\0" * (2 * 46)

    info = riff_list(
        b"INFO",
        chunk(b"ifil", struct.pack("<HH", 2, 1)),
        chunk(b"isng", b"EMU8000\0"),
        chunk(b"INAM", b"Rovox test sine\0"),
        chunk(b"ICOP", b"Own work of the Rovox project, AGPL-3.0-or-later\0"),
    )
    sdta = riff_list(b"sdta", chunk(b"smpl", smpl))

    # preset: "Test Sine", bank 0, program 0, one zone that points at the instrument
    phdr = name20("Test Sine") + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0) + name20("EOP") + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0)
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0)
    pmod = b"\0" * 10
    pgen = struct.pack("<HH", 41, 0) + struct.pack("<HH", 0, 0)                 # instrument 0
    # instrument: one zone, key range 0-127, loop continuously, sample 0
    inst = name20("Test Sine") + struct.pack("<H", 0) + name20("EOI") + struct.pack("<H", 1)
    ibag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 3, 0)
    imod = b"\0" * 10
    igen = struct.pack("<HBB", 43, 0, 127) + struct.pack("<Hh", 54, 1) + struct.pack("<HH", 53, 0) + struct.pack("<HH", 0, 0)
    shdr = (name20("sine440")
            + struct.pack("<IIIIIBbHH", 0, LENGTH, LOOP_START, LOOP_END, SAMPLE_RATE, ROOT_KEY, 0, 0, 1)
            + name20("EOS") + struct.pack("<IIIIIBbHH", 0, 0, 0, 0, 0, 0, 0, 0, 0))
    pdta = riff_list(
        b"pdta",
        chunk(b"phdr", phdr), chunk(b"pbag", pbag), chunk(b"pmod", pmod), chunk(b"pgen", pgen),
        chunk(b"inst", inst), chunk(b"ibag", ibag), chunk(b"imod", imod), chunk(b"igen", igen), chunk(b"shdr", shdr),
    )
    body = b"sfbk" + info + sdta + pdta
    return b"RIFF" + struct.pack("<I", len(body)) + body


def write_pack(out: Path, pack_id: str, sf2: bytes, license_id: str, server_use_ok, notes: str) -> None:
    directory = out / pack_id
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "sine.sf2").write_bytes(sf2)
    sha = hashlib.sha256(sf2).hexdigest()
    manifest = {
        "id": pack_id,
        "version": "1",
        "sha256": sha,
        "license": license_id,
        "server_use_ok": server_use_ok,
        "attribution": "Rovox test sine, own work",
        "source_url": "https://github.com/Alexshl/rovox",
        "license_notes": notes,
        "files": {"sine.sf2": sha},
        "instruments": [{"name": "Test Sine", "gm_program": 0, "bank": 0, "range": [0, 127], "tags": ["test", "sine"]}],
    }
    (directory / "pack.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("outdir", nargs="?", help="directory that gets the test packs")
    parser.add_argument("--sf2", help="write only the soundfont to this file")
    args = parser.parse_args()
    sf2 = build_sf2()

    if args.sf2:
        Path(args.sf2).write_bytes(sf2)
        return 0

    if not args.outdir:
        parser.error("give OUTDIR or --sf2 FILE")

    out = Path(args.outdir)
    write_pack(out, "rovox-test-sine", sf2, "AGPL-3.0-or-later", True, "Computed sine, own work.")
    write_pack(out, "rovox-test-unknown", sf2, "LicenseRef-unknown", "unknown", "The same sine, declared with an unknown licence for the licence-gate test.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
