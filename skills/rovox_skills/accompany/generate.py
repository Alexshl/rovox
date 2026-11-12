# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""The accompaniment generator: style packs (JSON) + chords (given or harmonised from the melody) + a seed -> note lists for bass, comp and drums.

Times are quarter notes. The output timeline is: count-in bars (a click), intro bars, then the body of `bars` bars; the chords and the melody are given relative to
the first body bar (bar 1, beat 1). The same inputs and seed give identical output; the seed only chooses among the variation slots of the style pack
(bar variants, fills, small velocity changes), never the harmony.
"""
import bisect
import json
import math
import os
import re

from . import harmony
from .prng import Rng

STYLES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "styles")
LEVELS = ("beginner", "intermediate")
ROLES = ("bass", "comp", "drums")
GM_DRUMS = {"kick": 36, "side": 37, "snare": 38, "hat": 42, "openhat": 46, "ride": 51, "crash": 49, "tom_low": 41, "tom_mid": 47, "tom_hi": 50, "click": 37}
_EPS = 1e-6
_NOTE_RE = re.compile(r"^([A-G])([#b]?)(-?\d)$")
_styles = None


def load_styles():
    """The style packs by id (every *.json in the styles directory)."""
    global _styles
    if _styles is None:
        packs = {}
        for name in sorted(os.listdir(STYLES_DIR)):
            if name.endswith(".json"):
                with open(os.path.join(STYLES_DIR, name), encoding="utf-8") as f:
                    p = json.load(f)
                packs[p["id"]] = p
        _styles = packs
    return _styles


def style_catalog():
    out = []
    for p in load_styles().values():
        out.append({"id": p["id"], "version": p["version"], "name": p["name"], "meters": p["meters"], "tempo_range": p["tempo_range"], "levels": sorted(p["levels"]),
                    "parts": {role: {lv: sorted(spec[role]) for lv, spec in p["levels"].items() if spec.get(role)} for role in ROLES}})
    return out


def parse_time_signature(ts):
    if isinstance(ts, str):
        m = re.match(r"^\s*(\d+)\s*/\s*(\d+)\s*$", ts)
        if not m:
            raise ValueError("cannot read the time signature %r (example: 3/4)" % ts)
        return int(m.group(1)), int(m.group(2))
    num, den = ts
    return int(num), int(den)


def _pitch(p):
    if isinstance(p, str):
        m = _NOTE_RE.match(p.strip())
        if not m:
            raise ValueError("cannot read the note name %r" % p)
        return 12 * (int(m.group(3)) + 1) + harmony._pc(m.group(1), m.group(2))
    return int(p)


def _melody(melody):
    out = []
    for n in melody or []:
        out.append({"pitch": _pitch(n["pitch"]), "start_qn": float(n["start_qn"]), "length_qn": float(n["length_qn"])})
    return sorted(out, key=lambda n: (n["start_qn"], n["pitch"]))


def _num(x):
    return int(x) if float(x) == int(x) else x


def _normalise_chords(chords, bar_qn, beat_qn, bars, warnings):
    """-> (output chords [{bar, beat, symbol}], [(start_qn, parsed chord)] with the first start at 0)."""
    items = []
    for c in chords:
        bar, beat = int(c["bar"]), float(c.get("beat", 1))
        if bar < 1 or beat < 1:
            raise ValueError("chord positions are 1-based (bar >= 1, beat >= 1): %r" % (c,))
        items.append((bar, beat, c["symbol"]))
    items.sort(key=lambda x: (x[0], x[1]))
    timeline, out, seen = [], [], {}
    for bar, beat, symbol in items:
        start = (bar - 1) * bar_qn + (beat - 1) * beat_qn
        if start >= bars * bar_qn - _EPS:
            warnings.append("chord %s at bar %d beat %s is after the last bar and is ignored" % (symbol, bar, _num(beat)))
            continue
        parsed = harmony.parse_chord(symbol)
        if parsed["warning"]:
            warnings.append(parsed["warning"])
        key = round(start / _EPS)
        if key in seen:        # two chords at the same time: the later one wins
            out[seen[key]] = {"bar": bar, "beat": _num(beat), "symbol": symbol}
            timeline[seen[key]] = (start, parsed)
            continue
        seen[key] = len(out)
        out.append({"bar": bar, "beat": _num(beat), "symbol": symbol})
        timeline.append((start, parsed))
    if not timeline:
        raise ValueError("no usable chords")
    if timeline[0][0] > _EPS:
        warnings.append("the first chord is at bar %d beat %s: %s is held from the start" % (out[0]["bar"], out[0]["beat"], out[0]["symbol"]))
        out.insert(0, {"bar": 1, "beat": 1, "symbol": out[0]["symbol"]})
        timeline.insert(0, (0.0, timeline[0][1]))
    return out, timeline


def _meter_pulse(num, den):
    """The pulse of the bar in quarter notes: the beat, or the dotted beat of compound meters (6/8, 9/8, 12/8)."""
    return (4.0 / den) * (3 if den == 8 and num % 3 == 0 and num > 3 else 1)


def _pick(voicing, pick):
    n = len(voicing)
    if pick == "all":
        return list(voicing)
    if pick == "upper":
        return list(voicing[1:])
    if pick == "top":
        return [voicing[-1]]
    return sorted({voicing[i % n] for i in pick})


def generate(melody=None, chords=None, key=None, time_signature="4/4", tempo=100, bars=None, style="pop-8th", level="beginner", seed=0, parts=None,
             count_in_bars=0, intro_bars=0, ending=True):
    """Generates the accompaniment. melody: [{pitch, start_qn, length_qn}] (quarter notes from body bar 1); chords: [{bar, beat, symbol}] (1-based; beat counts in
    the unit of the time signature's denominator) or None to harmonise the melody in `key`. Returns {tracks, chords, style, level, seed, tempo, time_signature,
    key, timeline, analysis, warnings}."""
    warnings = []
    num, den = parse_time_signature(time_signature)
    meter = "%d/%d" % (num, den)
    bar_qn, beat_qn = num * 4.0 / den, 4.0 / den
    packs = load_styles()
    if style not in packs:
        raise ValueError("unknown style %r (styles: %s)" % (style, ", ".join(sorted(packs))))
    pack = packs[style]
    if meter not in pack["meters"]:
        raise ValueError("style %s has no %s pattern (meters: %s)" % (style, meter, ", ".join(pack["meters"])))
    if level not in pack["levels"]:
        raise ValueError("unknown level %r (levels: %s)" % (level, ", ".join(sorted(pack["levels"]))))
    lvl = pack["levels"][level]
    seed, count_in, intro = int(seed), int(count_in_bars), int(intro_bars)
    if count_in < 0 or intro < 0 or count_in > 8 or intro > 8:
        raise ValueError("count_in_bars and intro_bars are 0..8")
    tempo = float(tempo)
    lo_t, hi_t = pack["tempo_range"]
    if not lo_t <= tempo <= hi_t:
        warnings.append("tempo %s is outside the usual %d-%d bpm of the %s style" % (_num(tempo), lo_t, hi_t, style))
    notes_in = _melody(melody)

    if bars is None:
        if notes_in:
            bars = int(math.ceil(max(n["start_qn"] + n["length_qn"] for n in notes_in) / bar_qn - _EPS))
        elif chords:
            bars = max(int(c["bar"]) for c in chords)
        else:
            raise ValueError("give bars, or a melody or chords to measure it from")
    bars = int(bars)
    if not 1 <= bars <= 512:
        raise ValueError("bars must be 1..512")

    analysis = {}
    if chords:
        out_chords, timeline = _normalise_chords(chords, bar_qn, beat_qn, bars, warnings)
    else:
        if not notes_in or not key:
            raise ValueError("without chords the harmoniser needs a melody and a key")
        inferred, confidence, hw = harmony.harmonise(notes_in, key, (num, den), bars, sevenths=(level != "beginner"))
        warnings.extend(hw)
        analysis["chord_confidence"] = confidence
        analysis["harmonised"] = True
        out_chords = [{"bar": c["bar"], "beat": c["beat"], "symbol": c["symbol"]} for c in inferred]
        timeline = [((c["bar"] - 1) * bar_qn, harmony.parse_chord(c["symbol"])) for c in inferred]
        # merge repeated chords so a held chord is one segment
        merged = [timeline[0]]
        for t, c in timeline[1:]:
            if c["symbol"] != merged[-1][1]["symbol"]:
                merged.append((t, c))
        timeline = merged

    # segments (chord spans), in body-relative quarter notes; the first one reaches back over the intro
    intro_qn, body_qn = intro * bar_qn, bars * bar_qn
    starts = [t for t, _ in timeline]
    segs = []
    for i, (t, c) in enumerate(timeline):
        segs.append({"start": t, "end": starts[i + 1] if i + 1 < len(starts) else body_qn, "chord": c})
    seg_keys = [-intro_qn] + starts[1:]

    def seg_at(t):
        return bisect.bisect_right(seg_keys, t + _EPS) - 1

    # voicings with voice leading; the comp stays below the lowest melody note of its span
    previous = None
    for s in segs:
        hi = harmony.COMP_HI
        span = [n["pitch"] for n in notes_in if n["start_qn"] < s["end"] - _EPS and n["start_qn"] + n["length_qn"] > s["start"] + _EPS]
        if span:
            hi = min(hi, min(span) - 1)
        s["voicing"], fitted = harmony.voice(s["chord"], previous, harmony.COMP_LO, hi, int(lvl.get("comp_tones", 0)) or None)
        if not fitted:
            warnings.append("bar %d: the melody lies too low to keep the comp below it; the comp may overlap the melody" % (int(s["start"] // bar_qn) + 1))
        previous = s["voicing"]

    wanted = list(parts) if parts else list(ROLES)
    for p in wanted:
        if p not in ROLES:
            raise ValueError("unknown part %r (parts: %s)" % (p, ", ".join(ROLES)))
    active = [p for p in ROLES if p in wanted and meter in lvl.get(p, {})]
    for p in wanted:
        if p not in active and parts:
            warnings.append("the %s style at the %s level has no %s part for %s: skipped" % (style, level, p, meter))

    fill_every = int(lvl.get("fill_every", 0))
    humanize = int(lvl.get("humanize_vel", 0))
    swing = float(pack.get("swing", 0.5))
    raw = {p: [] for p in active}
    for b in range(1 - intro, bars + 1):
        bar_start = (b - 1) * bar_qn
        bar_end = bar_start + bar_qn
        is_ending = bool(ending) and b == bars
        is_fill = (not is_ending) and (b < bars) and ((fill_every > 0 and b >= 1 and b % fill_every == 0) or (intro >= 1 and b == 0))
        prev_fill = (not is_ending) and ((fill_every > 0 and b > 1 and (b - 1) % fill_every == 0) or (intro >= 1 and b == 1))
        for part in active:
            spec = lvl[part][meter]
            rng = Rng.for_slot(seed, b, part)
            u = rng.random()
            variants, fills = spec.get("variants") or [], spec.get("fill") or []
            vi, fi = rng.below(max(1, len(variants))), rng.below(max(1, len(fills)))
            if is_ending:
                events = spec["ending"]
            elif is_fill and fills:
                events = fills[fi]
            else:
                events = spec["cycle"][(b - 1) % len(spec["cycle"])]
                if variants and u < spec.get("variant_prob", 0.0):
                    events = variants[vi]
            if prev_fill and spec.get("after_fill"):
                events = events + spec["after_fill"]
            for e in events:
                at = e["at"]
                if swing != 0.5 and abs((at % 1.0) - 0.5) < _EPS:
                    at += swing - 0.5
                t = bar_start + at
                end = min(t + e["len"], bar_end)
                vel = e["vel"] + (rng.randint(-humanize, humanize) if humanize else 0)
                vel = max(1, min(127, vel))
                if end - t < 0.03 or t >= bar_end - _EPS:
                    continue
                si = seg_at(t)
                if part == "bass":
                    nxt = segs[si + 1]["chord"] if si + 1 < len(segs) else None
                    raw[part].append([harmony.bass_pitch(segs[si]["chord"], e["role"], nxt), t, end, vel])
                elif part == "comp":
                    for pitch in _pick(segs[si]["voicing"], e["pick"]):
                        raw[part].append([pitch, t, end, vel])
                else:
                    raw[part].append([GM_DRUMS[e["drum"]], t, end, vel])

    result_notes = {}
    for part in active:
        notes = raw[part]
        if part != "drums":
            notes = _follow_chords(notes, part, segs, seg_keys, bar_qn)
        result_notes[part] = _no_overlaps(notes)

    off = (count_in + intro) * bar_qn
    tracks = []
    click = []
    if count_in:
        pulse = _meter_pulse(num, den)
        for i in range(int(round(bar_qn / pulse)) * count_in):
            first = i % int(round(bar_qn / pulse)) == 0
            click.append({"pitch": GM_DRUMS["click"], "start_qn": round(i * pulse, 6), "length_qn": 0.1, "velocity": 110 if first else 84})
    if click:
        tracks.append({"name": "Count-in", "role": "click", "program": 0, "channel": 9, "notes": click})
    for part in active:
        info = pack["parts"][part]
        notes = [{"pitch": int(p), "start_qn": round(s + off, 6), "length_qn": round(e - s, 6), "velocity": int(v)}
                 for p, s, e, v in sorted(result_notes[part], key=lambda n: (n[1], n[0]))]
        tracks.append({"name": info["name"], "role": part, "program": info["program"], "channel": info["channel"], "notes": notes})

    return {"tracks": tracks, "chords": out_chords, "style": {"id": pack["id"], "version": pack["version"]}, "level": level, "seed": seed, "tempo": _num(tempo),
            "time_signature": meter, "key": key, "analysis": analysis, "warnings": warnings,
            "timeline": {"bar_qn": bar_qn, "count_in_bars": count_in, "intro_bars": intro, "bars": bars, "body_start_qn": off, "total_qn": (count_in + intro + bars) * bar_qn}}


def _follow_chords(notes, part, segs, seg_keys, bar_qn):
    """Cuts notes at the next chord change and makes sure every chord change has an attack, so a chord changing mid-bar is heard on its beat."""
    out = []
    for p, s, e, v in notes:
        si = bisect.bisect_right(seg_keys, s + _EPS) - 1
        if part == "bass" and si > 0 and abs(s - segs[si]["start"]) < _EPS:
            p = harmony.bass_pitch(segs[si]["chord"], "root")      # a chord change is announced by its root
        out.append([p, s, min(e, segs[si]["end"]), v])
    for seg in segs[1:]:
        t = seg["start"]
        if any(abs(n[1] - t) < _EPS for n in out) or abs(t / bar_qn - round(t / bar_qn)) < _EPS:
            continue        # attacked already, or a change on the bar line, which the pattern of the bar handles
        later = [n[1] for n in out if n[1] > t + _EPS]
        bar_end = (math.floor(t / bar_qn + _EPS) + 1) * bar_qn
        end = min(seg["end"], bar_end, min(later) if later else bar_end)
        if end - t < 0.1:
            continue
        if part == "bass":
            out.append([harmony.bass_pitch(seg["chord"], "root"), t, end, 72])
        else:
            out.extend([p, t, end, 60] for p in seg["voicing"])
    return out


def _no_overlaps(notes):
    """No two notes of the same pitch overlap: the earlier one is shortened (or dropped if nothing is left)."""
    by_pitch = {}
    for n in sorted(notes, key=lambda n: (n[0], n[1])):
        lst = by_pitch.setdefault(n[0], [])
        if lst and lst[-1][2] > n[1] - _EPS:
            lst[-1][2] = n[1]
            if lst[-1][2] - lst[-1][1] < 0.03:
                lst.pop()
        lst.append(n)
    return [n for lst in by_pitch.values() for n in lst]
