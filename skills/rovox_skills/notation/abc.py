# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""ABC notation through the external `abc2midi` program (apt package abcmidi, run as a separate process) plus a small reader for the ABC header.

Full ABC parsing is out of scope: the header (X, T, M, L, K, Q) is validated here, abc2midi turns the tune into a Standard MIDI File and the first track with notes
becomes the melody.
"""
import math
import os
import re
import shutil
import struct
import subprocess
import tempfile

from .errors import invalid, unavailable
from .musicxml import estimate_key, key_name
from .paths import resolve_in
from .transpose import _tonic_fifths

MAX_BYTES = 1024 * 1024
TIMEOUT_S = 30
_MODES = {"": "major", "maj": "major", "major": "major", "ion": "major", "m": "minor", "min": "minor", "minor": "minor", "aeo": "minor", "dor": "dorian",
          "phr": "phrygian", "lyd": "lydian", "mix": "mixolydian", "loc": "locrian"}


def read_header(text):
    """Header fields of the first tune: {X, T (first title), M, L, K, Q} as raw strings. Raises INVALID_PARAMS without X: or K:."""
    fields, in_tune = {}, False
    for ln, raw in enumerate(text.splitlines(), 1):
        line = raw.split("%", 1)[0].rstrip()
        if not line.strip():
            if in_tune:
                break
            continue
        m = re.match(r"^([A-Za-z]):\s*(.*)$", line)
        if not m:
            if in_tune:
                break       # the tune body starts
            continue
        letter, value = m.group(1), m.group(2).strip()
        if letter == "X":
            if in_tune:
                break
            in_tune = True
        if in_tune and letter in "XTMLKQ":
            fields.setdefault(letter, value)
            if letter == "K":
                break       # K: ends the header
    if "X" not in fields:
        raise invalid("the ABC tune has no X: reference number line", element="X")
    if "K" not in fields:
        raise invalid("the ABC header has no K: key line", element="K")
    return fields


def parse_key(value):
    """K: field -> (tonic or None, mode, fifths or None)."""
    v = value.strip()
    if not v or v.lower().startswith("none"):
        return None, None, None
    m = re.match(r"^([A-G])([#b]?)\s*([A-Za-z]*)", v)
    if not m:
        raise invalid("unreadable key %r" % value, element="K")
    word = m.group(3).lower()
    mode = _MODES.get(word[:3] if word[:3] in _MODES else word, "major")      # other words (clef=..., exp) are not modes
    tonic = m.group(1) + m.group(2)
    return tonic, mode, _tonic_fifths(tonic, mode)


def parse_meter(value):
    v = value.strip()
    if v == "C":
        return 4, 4
    if v == "C|":
        return 2, 2
    m = re.match(r"^(\d+)\s*/\s*(\d+)$", v)
    if not m or int(m.group(1)) <= 0 or int(m.group(2)) <= 0:
        raise invalid("unreadable meter %r" % value, element="M")
    return int(m.group(1)), int(m.group(2))


def parse_tempo(value):
    """Q: field -> quarter notes per minute or None."""
    m = re.search(r"(\d+)\s*/\s*(\d+)\s*=\s*(\d+(?:\.\d+)?)", value or "")
    if m and int(m.group(2)):
        return float(m.group(3)) * 4 * int(m.group(1)) / int(m.group(2))
    m = re.search(r"(?:^|\s)(\d+(?:\.\d+)?)\s*$", value or "")
    return float(m.group(1)) if m else None


# -- a minimal Standard MIDI File reader -----------------------------------------------------------------------------
def read_smf(data):
    """Notes of the first track with notes (channel 10 skipped) as {pitch,start_qn,length_qn}, plus first tempo (qpm), time signature and key signature."""
    if data[:4] != b"MThd" or len(data) < 14:
        raise invalid("abc2midi produced no valid MIDI file")
    _fmt, ntrks, div = struct.unpack(">HHH", data[8:14])
    if div & 0x8000 or div == 0:
        raise invalid("SMPTE time division is not supported")
    pos = 8 + struct.unpack(">I", data[4:8])[0]
    meta = {"tempo": None, "time": None, "key": None}
    chosen = None
    for _ in range(ntrks):
        if data[pos:pos + 4] != b"MTrk" or pos + 8 > len(data):
            break
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        body, pos = data[pos + 8:pos + 8 + size], pos + 8 + size
        notes, on, t, i, status = [], {}, 0, 0, 0
        try:
            while i < len(body):
                delta = 0
                while True:
                    b = body[i]
                    i += 1
                    delta = (delta << 7) | (b & 0x7F)
                    if not b & 0x80:
                        break
                t += delta
                if body[i] & 0x80:
                    status = body[i]
                    i += 1
                if status == 0xFF:
                    mtype = body[i]
                    ln = body[i + 1]
                    payload = body[i + 2:i + 2 + ln]
                    i += 2 + ln
                    if mtype == 0x51 and ln == 3 and meta["tempo"] is None:
                        meta["tempo"] = 60000000 / int.from_bytes(payload, "big")
                    elif mtype == 0x58 and ln >= 2 and meta["time"] is None:
                        meta["time"] = (payload[0], 2 ** payload[1])
                    elif mtype == 0x59 and ln == 2 and meta["key"] is None:
                        meta["key"] = (struct.unpack("b", payload[:1])[0], "minor" if payload[1] else "major")
                elif status in (0xF0, 0xF7):
                    ln = body[i]
                    i += 1 + ln
                else:
                    kind, ch = status & 0xF0, status & 0x0F
                    n = 1 if kind in (0xC0, 0xD0) else 2
                    d = body[i:i + n]
                    i += n
                    if kind == 0x90 and d[1] > 0 and ch != 9:
                        on.setdefault((ch, d[0]), []).append(t)
                    elif kind in (0x80, 0x90) and ch != 9 and on.get((ch, d[0])):
                        s = on[(ch, d[0])].pop(0)
                        if t > s:
                            notes.append({"pitch": d[0], "start_qn": round(s / div, 6), "length_qn": round((t - s) / div, 6)})
        except IndexError:
            raise invalid("abc2midi produced a damaged MIDI file")
        if chosen is None and notes:
            chosen = sorted(notes, key=lambda n: (n["start_qn"], n["pitch"]))
    return chosen or [], meta["tempo"], meta["time"], meta["key"]


# -- the import ----------------------------------------------------------------------------------------------------
def available():
    return shutil.which("abc2midi") is not None


def import_abc(path):
    real = resolve_in(path)
    try:
        with open(real, "rb") as f:
            raw = f.read(MAX_BYTES + 1)
    except OSError as e:
        raise invalid("cannot read the file: %s" % e.strerror, file=path)
    if len(raw) > MAX_BYTES:
        raise invalid("the ABC file is larger than 1 MB")
    text = raw.decode("utf-8", errors="replace")
    head = read_header(text)         # validated first so a broken file is INVALID_PARAMS even without the program
    tonic, mode, fifths = parse_key(head["K"])
    meter = parse_meter(head["M"]) if "M" in head else None
    exe = shutil.which("abc2midi")
    if exe is None:
        raise unavailable("ABC import needs abc2midi (apt package abcmidi, part of the skills-notation image), which is not installed")
    with tempfile.TemporaryDirectory(prefix="rovox-abc-") as tmp:
        src, out = os.path.join(tmp, "in.abc"), os.path.join(tmp, "out.mid")
        with open(src, "wb") as f:
            f.write(raw)
        try:
            proc = subprocess.run([exe, src, head["X"].split()[0], "-o", out], cwd=tmp, env={"PATH": os.environ.get("PATH", "")}, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            raise invalid("abc2midi did not finish in %d s" % TIMEOUT_S)
        except OSError as e:
            raise unavailable("abc2midi could not be started: %s" % e.strerror)
        if proc.returncode != 0 or not os.path.exists(out):
            raise invalid("abc2midi could not convert the tune: %s" % (proc.stderr.decode(errors="replace").strip()[:300] or "no output"))
        with open(out, "rb") as f:
            notes, smf_tempo, smf_time, smf_key = read_smf(f.read(MAX_BYTES * 8))
    warnings = ["ABC import keeps the first voice only; chord symbols and lyrics are ignored"]
    tempo = parse_tempo(head.get("Q")) or smf_tempo
    meter = meter or smf_time or (4, 4)
    length = max((n["start_qn"] + n["length_qn"] for n in notes), default=0.0)
    if tonic is not None:
        keyinfo = {"key": key_name(fifths, mode), "mode": mode, "key_fifths": fifths, "key_confidence": 1.0, "key_source": "signature"}
    else:
        est = estimate_key(notes)
        keyinfo = ({"key": est["key"], "mode": est["mode"], "key_fifths": est["fifths"], "key_confidence": est["confidence"], "key_source": "estimated"} if est
                   else {"key": None, "mode": None, "key_fifths": None, "key_confidence": 0.0, "key_source": "none"})
        warnings.append("K:none in the ABC header: the key is estimated")
    if not notes:
        warnings.append("the tune has no notes")
    result = {"format": "abc", "title": head.get("T") or None, "parts": [{"index": 1, "id": "1", "name": head.get("T") or "", "midi_program": None}], "part": "1"}
    result.update(keyinfo)
    result.update({"time_signature": {"beats": meter[0], "beat_type": meter[1]}, "tempo": round(tempo, 3) if tempo else None, "tempo_changes": [], "pickup_qn": 0.0,
                   "bars": int(math.ceil(length / (meter[0] * 4 / meter[1]) - 1e-9)), "length_qn": round(length, 6), "expanded_repeats": True, "transposed_semitones": 0,
                   "melody": {"notes": notes}, "chords": [], "warnings": warnings})
    return result
