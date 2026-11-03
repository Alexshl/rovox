#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Converts the DrumGizmo kit DRSKit (CC BY 4.0) into a Rovox drum kit for the built-in sampler `rovox:drums`.

Offline tool, not part of the server or of the Docker image. Needs Python 3.9+, numpy and soundfile (libsndfile with FLAC).

  1. Download DRSKit 2.1 (about 2.8 GB) from https://drumgizmo.org/wiki/doku.php?id=kits:drskit
     (direct link: https://drumgizmo.org/kits/DRSKit/DRSKit2_1.zip), license CC BY 4.0.
  2. tools/drumkit/build_drskit.py DRSKit2_1.zip --out /path/to/drumkits          (a zip or the unpacked directory)
  3. The result is /path/to/drumkits/drskit/{kit.json, <piece>/<layer>_<n>.flac}.
       - local / development:  ROVOX_DRUMKITS_PATH=/path/to/drumkits
       - object storage:       --stage-s3 /path/to/stage --prefix rovox/ builds <prefix>assets/drumkits/drskit/<sha>/ (hard links) and a LATEST file; upload the
                               stage directory to the bucket (`aws s3 sync` / `mc mirror`). The server downloads the kit on first use into its cache.

Per hit the 13 microphones of the multichannel recording are mixed down to one stereo sample (overheads and room wide, close mics at the drum's place
in the kit, snare bottom and kick pair phase-aligned), cut at the onset, trimmed at the tail with a short fade, and the whole kit is scaled once so that
its loudest hit peaks at --peak-db. The hits of an instrument, sorted by the power DrumGizmo measured, become velocity layers with round robin.
Nothing from the kit is committed to the Rovox repository."""
import argparse
import hashlib
import io
import json
import math
import os
import shutil
import sys
import xml.etree.ElementTree as ET
import zipfile

import numpy as np
import soundfile as sf

KIT_ID = "drskit"
KIT_NAME = "DRSKit"
KIT_VERSION = "2.1"
LICENSE = "CC-BY-4.0"
SOURCE_URL = "https://drumgizmo.org/wiki/doku.php?id=kits:drskit"
ATTRIBUTION = "Drum samples: DRSKit by the DrumGizmo team and Jes Eiler (DRSDrums), CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)"

# Default mic balance: (linear gain, pan -1 = left .. +1 = right, as the drummer hears the kit). Overheads and room give the stereo picture, the close
# mics add body at the position of their drum.
MIC_MIX = {
    "AmbL": (0.35, -1.0), "AmbR": (0.35, 1.0),
    "OHL": (0.70, -0.9), "OHR": (0.70, 0.9),
    "Kdrum_back": (0.50, 0.0), "Kdrum_front": (0.50, 0.0),
    "Snare_top": (0.60, 0.1), "Snare_bottom": (0.25, 0.1),
    "Hihat": (0.45, -0.5), "Ride": (0.40, 0.5),
    "Tom1": (0.50, -0.35), "Tom2": (0.50, 0.1), "Tom3": (0.50, 0.5),
}
POLARITY_PAIRS = [("Snare_bottom", "Snare_top"), ("Kdrum_back", "Kdrum_front")]      # the first is flipped if it correlates negatively


class Piece:
    def __init__(self, pid, name, category, notes, instrument, max_s, group="", chokes=(), max_voices=4):
        self.id, self.name, self.category, self.notes, self.instrument = pid, name, category, list(notes), instrument
        self.max_s, self.group, self.chokes, self.max_voices = max_s, group, list(chokes), max_voices


# GM drum map as MT-PowerDrumKit uses it. DRSKit has no china, splash, cowbell or tambourine: they are `fallbacks` below.
PIECES = [
    Piece("kick", "Kick", "kick", [35, 36], "Kdrum_with_contact", 1.2, max_voices=3),
    Piece("snare", "Snare", "snare", [38, 40], "Snare", 1.6, max_voices=4),
    Piece("sidestick", "Side stick", "snare", [37], "Snare_rim", 0.9, max_voices=3),
    Piece("hh_closed", "Hi-hat closed", "hihat", [42], "Hihat_closed", 0.9, chokes=["hat_open"], max_voices=4),
    Piece("hh_pedal", "Hi-hat pedal", "hihat", [44], "Hihat_foot", 0.6, chokes=["hat_open"], max_voices=3),
    Piece("hh_open", "Hi-hat open", "hihat", [46], "Hihat_open", 3.5, group="hat_open", max_voices=2),
    Piece("tom_floor_low", "Floor tom", "tom", [41, 43], "Tom3", 2.8, max_voices=3),
    Piece("tom_mid", "Mid tom", "tom", [45, 47], "Tom2", 2.5, max_voices=3),
    Piece("tom_high", "High tom", "tom", [48, 50], "Tom1", 2.2, max_voices=3),
    Piece("crash_left", "Crash left", "cymbal", [49], "Crash_left_tip", 5.5, max_voices=2),
    Piece("crash_right", "Crash right", "cymbal", [57], "Crash_right_tip", 5.5, max_voices=2),
    Piece("ride", "Ride", "cymbal", [51, 59], "Ride_tip", 4.5, max_voices=3),
    Piece("ride_bell", "Ride bell", "cymbal", [53], "Ride_tip_bell", 4.0, max_voices=2),
]
FALLBACKS = {52: "crash_right", 55: "crash_left", 56: "ride_bell", 54: "hh_closed"}      # china, splash, cowbell, tambourine


class Source:
    """A DrumGizmo kit as a directory or a zip file."""

    def __init__(self, path):
        self.path = path
        self.zip = zipfile.ZipFile(path) if os.path.isfile(path) else None
        names = self.zip.namelist() if self.zip else []
        self.root = ""

        if self.zip:
            kits = [n for n in names if n.endswith("DRSKit_full.xml")]
            if not kits:
                sys.exit("no DRSKit_full.xml in " + path)
            self.root = kits[0][: -len("DRSKit_full.xml")]
        else:
            for cand in (path, os.path.join(path, "DRSKit")):
                if os.path.isfile(os.path.join(cand, "DRSKit_full.xml")):
                    self.root = cand.rstrip("/") + "/"
                    break
            else:
                sys.exit("no DRSKit_full.xml in " + path)

    def read(self, rel):
        if self.zip:
            return self.zip.read(self.root + rel)
        with open(self.root + rel, "rb") as f:
            return f.read()


def parse_instrument(src, name):
    """[(sample name, power, {channel: (file, file channel index 0-based)})] of one instrument, from <name>/<name>.xml."""
    root = ET.fromstring(src.read("%s/%s.xml" % (name, name)))
    out = []
    for s in root.iter("sample"):
        chans = {}
        for a in s.iter("audiofile"):
            chans[a.get("channel")] = (name + "/" + a.get("file"), int(a.get("filechannel")) - 1)
        out.append((s.get("name"), float(s.get("power", "0")), chans))
    out.sort(key=lambda t: t[1])
    return out


def mixdown(src, chans, cache):
    """Stereo float32 (frames, 2) and the sample rate of one hit."""
    data, rate = {}, None
    for ch, (file, idx) in chans.items():
        if ch not in MIC_MIX:
            continue
        if file not in cache:
            cache.clear()                                    # one multichannel file at a time
            cache[file] = sf.read(io.BytesIO(src.read(file)), dtype="float32", always_2d=True)
        audio, rate = cache[file]
        data[ch] = audio[:, idx]
    for flip, ref in POLARITY_PAIRS:
        if flip in data and ref in data and float(np.dot(data[flip], data[ref])) < 0.0:
            data[flip] = -data[flip]
    frames = max(len(v) for v in data.values())
    out = np.zeros((frames, 2), dtype=np.float32)
    for ch, v in data.items():
        gain, pan = MIC_MIX[ch]
        angle = (pan + 1.0) * math.pi / 4.0                  # constant power pan
        out[: len(v), 0] += v * gain * math.cos(angle) * math.sqrt(2.0)
        out[: len(v), 1] += v * gain * math.sin(angle) * math.sqrt(2.0)
    return out, int(rate)


def shape(x, rate, max_s):
    """Cut the silence before the hit (2 ms of pre-roll stay), trim the tail below -72 dBFS, cap at max_s, fade the end."""
    mono = np.max(np.abs(x), axis=1)
    peak = float(mono.max())
    if peak <= 0.0:
        return x[:1]
    start = max(0, int(np.argmax(mono > peak * 0.01)) - int(0.002 * rate))
    x = x[start:]
    mono = mono[start:]
    end = min(len(x), int(max_s * rate))
    win = max(1, int(0.01 * rate))
    loud = np.nonzero(np.add.reduceat(mono[:end], np.arange(0, end, win)) / win > 10 ** (-72 / 20))[0]
    if len(loud):
        end = min(end, (int(loud[-1]) + 1) * win)
    x = x[:end].copy()
    fade = min(len(x), int(0.04 * rate))
    if fade > 1:
        x[-fade:] *= np.linspace(1.0, 0.0, fade, dtype=np.float32)[:, None]
    return x


def split_layers(count, max_layers, per_layer):
    """Sizes of consecutive groups for `count` power-sorted hits: as many layers as possible up to max_layers, at most per_layer hits in each."""
    layers = max(1, min(max_layers, count))
    base, extra = divmod(count, layers)
    sizes = [base + (1 if i < extra else 0) for i in range(layers)]
    return [min(s, per_layer) for s in sizes], sizes


def main():
    ap = argparse.ArgumentParser(description="DrumGizmo DRSKit -> Rovox drum kit (stereo FLAC + kit.json)")
    ap.add_argument("kit", help="DRSKit2_1.zip or the unpacked DRSKit directory")
    ap.add_argument("--out", required=True, help="output directory; the kit is written to <out>/drskit/ (point ROVOX_DRUMKITS_PATH at <out>)")
    ap.add_argument("--layers", type=int, default=6, help="velocity layers per instrument (default 6)")
    ap.add_argument("--round-robin", type=int, default=2, help="hits kept per layer, played in turn (default 2)")
    ap.add_argument("--peak-db", type=float, default=-1.0, help="the loudest hit of the kit peaks here (default -1 dBFS)")
    ap.add_argument("--stage-s3", metavar="DIR", help="also build DIR/<prefix>assets/drumkits/drskit/<sha>/ (hard links) and LATEST for upload to the bucket")
    ap.add_argument("--prefix", default="rovox/", help="S3_PREFIX of the server for --stage-s3 (default rovox/)")
    ap.add_argument("--list", action="store_true", help="only list the instruments of the kit and the pieces that use them")
    args = ap.parse_args()

    src = Source(args.kit)
    kit_xml = ET.fromstring(src.read("DRSKit_full.xml"))
    available = {i.get("name") for i in kit_xml.iter("instrument")}
    if args.list:
        for p in PIECES:
            print("%-14s notes %-10s <- %s%s" % (p.id, p.notes, p.instrument, "" if p.instrument in available else "   (MISSING in this kit)"))
        print("kit instruments:", ", ".join(sorted(available)))
        return
    missing = [p.instrument for p in PIECES if p.instrument not in available]
    if missing:
        sys.exit("instruments not found in the kit: " + ", ".join(missing))

    out_dir = os.path.join(args.out, KIT_ID)
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)

    # pass 1: mix every selected hit down, keep it in memory (about 100 MB)
    plan, peak, rate_seen = [], 0.0, None
    cache = {}
    for p in PIECES:
        hits = parse_instrument(src, p.instrument)
        per_layer, sizes = split_layers(len(hits), args.layers, args.round_robin)
        pos, layers = 0, []
        for size, keep in zip(sizes, per_layer):
            group = hits[pos: pos + size]
            pos += size
            # spread the kept hits over the group (the middle ones for a group of three and two kept)
            picks = sorted({int(round(k * (len(group) - 1) / max(1, keep - 1))) for k in range(keep)}) if keep > 1 else [len(group) // 2]
            layer = []
            for k in picks:
                name, power, chans = group[k]
                audio, rate = mixdown(src, chans, cache)
                rate_seen = rate_seen or rate
                if rate != rate_seen:
                    sys.exit("sample rates differ in the kit (%d / %d)" % (rate, rate_seen))
                audio = shape(audio, rate, p.max_s)
                peak = max(peak, float(np.abs(audio).max()))
                layer.append((name, power, audio))
            layers.append(layer)
        plan.append((p, layers))
        print("%-14s %2d hits -> %d layers, %d samples" % (p.id, len(hits), len(layers), sum(len(l) for l in layers)), flush=True)

    scale = 10 ** (args.peak_db / 20.0) / peak if peak > 0 else 1.0

    # pass 2: write FLAC and the manifest
    pieces, files = [], {}
    for p, layers in plan:
        total = len(layers)
        out_layers = []
        for li, layer in enumerate(layers):
            vmin = 1 + (127 * li) // total
            vmax = (127 * (li + 1)) // total
            names = []
            for ri, (name, power, audio) in enumerate(layer):
                rel = "%s/l%02d_%d.flac" % (p.id, li, ri)
                path = os.path.join(out_dir, rel)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                sf.write(path, np.clip(audio * scale, -1.0, 1.0), rate_seen, format="FLAC", subtype="PCM_16")
                files[rel] = os.path.getsize(path)
                names.append(rel)
            out_layers.append({"vel_min": vmin, "vel_max": vmax, "samples": names})
        pieces.append({"id": p.id, "name": p.name, "category": p.category, "notes": p.notes, "gain_db": 0.0, "pan": 0.0,
                       "choke_group": p.group, "chokes": p.chokes, "max_voices": p.max_voices, "layers": out_layers})

    manifest = {"format": "rovox-drumkit", "version": 1, "id": KIT_ID, "name": KIT_NAME, "kit_version": KIT_VERSION, "license": LICENSE,
                "attribution": ATTRIBUTION, "source": SOURCE_URL, "sample_rate": rate_seen, "pieces": pieces,
                "fallbacks": {str(k): v for k, v in sorted(FALLBACKS.items())}, "files": files}
    text = json.dumps(manifest, indent=1).encode("utf-8")
    with open(os.path.join(out_dir, "kit.json"), "wb") as f:
        f.write(text)
    sha = hashlib.sha256(text).hexdigest()
    size = sum(files.values()) + len(text)
    print("\nkit written to %s: %d samples, %.1f MB, sha256(kit.json) = %s" % (out_dir, len(files), size / 1e6, sha))
    print("local use:  ROVOX_DRUMKITS_PATH=%s" % os.path.abspath(args.out))

    if args.stage_s3:
        prefix = args.prefix.lstrip("/")
        if prefix and not prefix.endswith("/"):
            prefix += "/"
        base = os.path.join(args.stage_s3, prefix + "assets/drumkits/%s" % KIT_ID)
        dest = os.path.join(base, sha)
        shutil.rmtree(dest, ignore_errors=True)
        for root, _, names in os.walk(out_dir):
            for n in names:
                rel = os.path.relpath(os.path.join(root, n), out_dir)
                target = os.path.join(dest, rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                try:
                    os.link(os.path.join(root, n), target)
                except OSError:
                    shutil.copy2(os.path.join(root, n), target)
        with open(os.path.join(base, "LATEST"), "w") as f:
            f.write(sha + "\n")
        print("staged for upload: %s  (upload the contents of %s to the bucket root; upload the sha directory first, LATEST last)" % (base, args.stage_s3))


if __name__ == "__main__":
    main()
