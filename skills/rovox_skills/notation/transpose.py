# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Transposition for transposing instruments and a minimal MusicXML writer (one part) that LilyPond and verovio can engrave.

`semitones` is what is added to a concert (sounding) pitch to get the pitch the player reads: a Bb clarinet reads a major 2nd (+2) above what sounds.
"""
import re
from xml.sax.saxutils import escape

from . import musicxml
from .errors import invalid
from .musicxml import estimate_key, key_name, shift_fifths, spell_fifths
from .paths import resolve_in, resolve_out

# written = concert + semitones (en.wikipedia.org/wiki/Transposing_instrument); tenor/baritone sax and English horn are deliberately absent
INSTRUMENTS = {
    "C": 0, "concert": 0,
    "Bb": 2, "bb": 2, "clarinet": 2, "bb_clarinet": 2, "trumpet": 2, "bb_trumpet": 2, "soprano_sax": 2,
    "Eb": 9, "eb": 9, "alto_sax": 9, "eb_alto_sax": 9,
    "F": 7, "f": 7, "horn": 7, "f_horn": 7, "french_horn": 7,
    "piccolo": -12, "soprano_recorder": -12,
}
_NAMES_SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
_NAMES_FLAT = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
_CHORD = re.compile(r"^([A-G])([#b]*)(.*?)(?:/([A-G])([#b]*))?$")
_SUFFIX_KIND = {"": "major", "m": "minor", "7": "dominant", "maj7": "major-seventh", "m7": "minor-seventh", "dim": "diminished", "aug": "augmented",
                "m7b5": "half-diminished", "dim7": "diminished-seventh", "sus4": "suspended-fourth", "sus2": "suspended-second", "6": "major-sixth",
                "m6": "minor-sixth", "9": "dominant-ninth"}
TICKS = 24      # divisions per quarter note: 16th = 6, 32nd = 3, eighth triplet = 8, 16th triplet = 4


def resolve_target(to, octave=0):
    """-> (semitones, label) from "C"|"Bb"|"Eb"|"F"|an instrument name|an integer number of semitones, plus octave shifts."""
    if isinstance(octave, bool) or not isinstance(octave, int) or not -4 <= octave <= 4:
        raise invalid("octave must be an integer between -4 and 4", element="octave")
    if isinstance(to, bool) or to is None:
        raise invalid("to must be an instrument (C, Bb, Eb, F, ...) or a number of semitones", element="to")
    if isinstance(to, (int, float)):
        if to != int(to):
            raise invalid("to must be a whole number of semitones", element="to")
        semis, label = int(to), "%+d semitones" % int(to)
    else:
        s = str(to).strip()
        if s in INSTRUMENTS or s.lower() in INSTRUMENTS:
            semis, label = INSTRUMENTS[s if s in INSTRUMENTS else s.lower()], s
        elif re.fullmatch(r"[+-]?\d+", s):
            semis, label = int(s), "%+d semitones" % int(s)
        else:
            raise invalid("unknown instrument %r; known: %s" % (to, ", ".join(sorted(set(INSTRUMENTS)))), element="to")
    if not -48 <= semis <= 48:
        raise invalid("transposition by more than four octaves", element="to")
    return semis + 12 * octave, label


def transpose_chord(symbol, semitones, fifths=0):
    """Chord symbol moved up by semitones, spelled with flats when the target key has flats."""
    m = _CHORD.match(symbol or "")
    if not m:
        return symbol
    names = _NAMES_FLAT if fifths < 0 else _NAMES_SHARP

    def move(letter, acc):
        pc = _PC[letter] + acc.count("#") - acc.count("b")
        return names[(pc + semitones) % 12]
    out = move(m.group(1), m.group(2)) + m.group(3)
    if m.group(4):
        out += "/" + move(m.group(4), m.group(5))
    return out


_LINE = {"F": -1, "C": 0, "G": 1, "D": 2, "A": 3, "E": 4, "B": 5}


def _tonic_fifths(key, mode):
    """Key signature of a tonic name such as "G" or "Bb" in a mode: the tonic's place on the line of fifths minus the mode shift."""
    m = re.fullmatch(r"([A-G])(#|b)?", key or "")
    if not m:
        return None
    pos = _LINE[m.group(1)] + (7 if m.group(2) == "#" else -7 if m.group(2) == "b" else 0)
    return pos - musicxml._MODE_SHIFT.get(mode or "major", 0)


def transpose_score(info, semitones):
    """Transposes an import result (or {melody:{notes}, chords, key...}): notes, key signature and chord symbols. Returns a new dict."""
    notes = (info.get("melody") or {}).get("notes") or []
    warnings = list(info.get("warnings") or [])
    out_notes = []
    for n in notes:
        p = n["pitch"] + semitones
        if not 0 <= p <= 127:
            raise invalid("transposing by %+d semitones moves a note outside the MIDI range 0..127" % semitones, element="pitch")
        out_notes.append(dict(n, pitch=p))
    low, high = min((n["pitch"] for n in out_notes), default=60), max((n["pitch"] for n in out_notes), default=60)
    if low < 21 or high > 108:
        warnings.append("the transposed part leaves the range of a piano (A0..C8)")
    fifths = info.get("key_fifths")
    mode = info.get("mode") or "major"
    if fifths is None and info.get("key"):
        fifths = _tonic_fifths(info["key"], mode)
    if fifths is None:
        est = estimate_key(notes)
        if est:
            fifths, mode = est["fifths"], est["mode"]
            warnings.append("the key was estimated from the notes")
    new_fifths = shift_fifths(fifths, semitones) if fifths is not None else None
    chords = [dict(c, symbol=transpose_chord(c["symbol"], semitones, new_fifths or 0)) for c in info.get("chords") or []]
    out = dict(info)
    out.update({"melody": {"notes": out_notes}, "chords": chords, "warnings": warnings, "semitones": semitones, "key_fifths": new_fifths, "mode": mode if fifths is not None else info.get("mode"),
                "key": key_name(new_fifths, mode) if new_fifths is not None else None})
    return out


# -- MusicXML writer ------------------------------------------------------------------------------------------------
_NORMAL = [(96, "whole", 0), (72, "half", 1), (48, "half", 0), (36, "quarter", 1), (24, "quarter", 0), (18, "eighth", 1), (12, "eighth", 0), (9, "16th", 1), (6, "16th", 0), (3, "32nd", 0)]
_TUPLET = [(32, "half", 0), (16, "quarter", 0), (8, "eighth", 0), (4, "16th", 0), (2, "32nd", 0)]      # written with <time-modification> 3:2


def _decompose(ticks):
    """Ticks -> [(ticks, type, dots, tuplet)] and the ticks that could not be written."""
    pieces = []
    table_tuplet = ticks % 3 != 0
    while ticks > 0:
        table = _TUPLET if table_tuplet else _NORMAL
        pick = next((p for p in table if p[0] <= ticks), None)
        if pick is None:
            break
        pieces.append((pick[0], pick[1], pick[2], table_tuplet))
        ticks -= pick[0]
    return pieces, ticks


def _spell(midi, fifths):
    names = _NAMES_FLAT if fifths < 0 else _NAMES_SHARP
    name = names[midi % 12]
    return name[0], (1 if "#" in name else -1 if "b" in name else 0), midi // 12 - 1


def _bar_starts(total, pickup, bar_len):
    starts, t = [], 0
    if pickup:
        starts.append((0, pickup))
        t = pickup
    while t < total or not starts:
        starts.append((t, bar_len))
        t += bar_len
    return starts


def write_musicxml(notes, key_fifths=0, mode="major", time_signature=None, tempo=None, pickup_qn=0.0, chords=None, title=None):
    """One-part MusicXML 4.0 (partwise) of notes {pitch,start_qn,length_qn}; notes at the same start form a chord, overlaps are cut at the next onset. Returns (xml, warnings)."""
    warnings = []
    ts = time_signature or {"beats": 4, "beat_type": 4}
    beats, beat_type = int(ts["beats"]), int(ts["beat_type"])
    bar_len = beats * TICKS * 4 // beat_type
    fifths = int(key_fifths or 0)
    pickup = int(round(float(pickup_qn or 0) * TICKS))
    if pickup >= bar_len:
        pickup = 0
    groups = {}
    for n in notes:
        s = int(round(n["start_qn"] * TICKS))
        e = s + max(1, int(round(n["length_qn"] * TICKS)))
        groups.setdefault(s, []).append((e, n["pitch"]))
    starts = sorted(groups)
    segments = []       # [start, end, [pitches]]
    cut = 0
    for i, s in enumerate(starts):
        e = min(e for e, _ in groups[s])
        if i + 1 < len(starts) and e > starts[i + 1]:
            e = starts[i + 1]
            cut += 1
        segments.append([s, e, sorted({p for _, p in groups[s]})])
    if cut:
        warnings.append("%d overlapping notes were shortened: the engraving shows one melodic line plus chords" % cut)
    total = max((s[1] for s in segments), default=0)
    bars = _bar_starts(total, pickup, bar_len)
    lost = 0
    chord_ticks = []
    for c in chords or []:
        bi = c["bar"] if pickup else c["bar"] - 1
        if 0 <= bi < len(bars):
            chord_ticks.append((bars[bi][0] + int(round((c["beat"] - 1) * TICKS * 4 / beat_type)), bi, c["symbol"]))
    mean = sum(p for s in segments for p in s[2]) / max(1, sum(len(s[2]) for s in segments))
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<!DOCTYPE score-partwise PUBLIC "-//Recordare//DTD MusicXML 4.0 Partwise//EN" "http://www.musicxml.org/dtds/partwise.dtd">',
           '<score-partwise version="4.0">']
    if title:
        out.append("<work><work-title>%s</work-title></work>" % escape(str(title)))
    out.append('<part-list><score-part id="P1"><part-name>Part</part-name></score-part></part-list><part id="P1">')
    bar_no = 0
    for bi, (bs, blen) in enumerate(bars):
        be = bs + blen
        implicit = pickup and bi == 0
        if implicit:
            out.append('<measure number="0" implicit="yes">')
        else:
            bar_no += 1
            out.append('<measure number="%d">' % bar_no)
        if bi == 0:
            clef = ("F", 4) if mean < 55 else ("G", 2)
            out.append("<attributes><divisions>%d</divisions><key><fifths>%d</fifths><mode>%s</mode></key><time><beats>%d</beats><beat-type>%d</beat-type></time>"
                       "<clef><sign>%s</sign><line>%d</line></clef></attributes>" % (TICKS, fifths, escape(mode or "major"), beats, beat_type, clef[0], clef[1]))
            if tempo:
                out.append('<direction placement="above"><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>%g</per-minute></metronome></direction-type>'
                           '<sound tempo="%g"/></direction>' % (tempo, tempo))
        pieces = []     # (start, ticks, pitches or None for a rest, type, dots, tuplet, tie_prev, tie_next)
        cursor = bs
        spans = [(s[0], s[1], s[2]) for s in segments if s[1] > bs and s[0] < be]
        for s, e, pitches in spans:
            cs, ce = max(s, bs), min(e, be)
            if cs > cursor:
                lost += _add(pieces, cursor, cs, None, False, False)
            lost += _add(pieces, cs, ce, pitches, s < cs, e > ce)
            cursor = ce
        if cursor < be:
            lost += _add(pieces, cursor, be, None, False, False)
        ctr = 0
        placed = sorted((t, sym) for t, bar, sym in chord_ticks if bar == bi)
        for p in pieces:
            start, ticks, pitches, typ, dots, tup, tie_prev, tie_next = p
            while placed and placed[0][0] <= start:
                out.append(_harmony(placed.pop(0)[1]))
            if tup:
                tpos = ctr % 3
                ctr += 1
            else:
                ctr = 0
            for k, pitch in enumerate(pitches or [None]):
                x = ["<note>"]
                if k:
                    x.append("<chord/>")
                if pitch is None:
                    x.append("<rest/>")
                else:
                    step, alter, octave = _spell(pitch, fifths)
                    x.append("<pitch><step>%s</step>%s<octave>%d</octave></pitch>" % (step, "<alter>%d</alter>" % alter if alter else "", octave))
                x.append("<duration>%d</duration>" % ticks)
                if pitch is not None:
                    if tie_prev:
                        x.append('<tie type="stop"/>')
                    if tie_next:
                        x.append('<tie type="start"/>')
                x.append("<voice>1</voice><type>%s</type>%s" % (typ, "<dot/>" * dots))
                if tup:
                    x.append("<time-modification><actual-notes>3</actual-notes><normal-notes>2</normal-notes></time-modification>")
                notations = ""
                if pitch is not None:
                    if tie_prev:
                        notations += '<tied type="stop"/>'
                    if tie_next:
                        notations += '<tied type="start"/>'
                if tup and k == 0:
                    if tpos == 0:
                        notations += '<tuplet type="start" bracket="yes"/>'
                    elif tpos == 2:
                        notations += '<tuplet type="stop"/>'
                if notations:
                    x.append("<notations>%s</notations>" % notations)
                x.append("</note>")
                out.append("".join(x))
        for _, sym in placed:
            out.append(_harmony(sym))
        out.append("</measure>")
    out.append("</part></score-partwise>")
    if lost:
        warnings.append("%d durations could not be written exactly and were rounded" % lost)
    return "\n".join(out), warnings


def _add(pieces, s, e, pitches, tie_prev, tie_next):
    """Appends the written pieces of [s, e) and returns 1 when some ticks could not be written exactly."""
    parts, rest = _decompose(e - s)
    pos = s
    for i, (ticks, typ, dots, tup) in enumerate(parts):
        pieces.append((pos, ticks, pitches, typ, dots, tup, tie_prev or i > 0, tie_next or i < len(parts) - 1))
        pos += ticks
    if not parts:       # 1 tick: nothing writable, a rest of the smallest value keeps the bar readable
        pieces.append((s, e - s, None, "32nd", 0, False, False, False))
    return 1 if rest else 0


def _harmony(symbol):
    m = _CHORD.match(symbol or "")
    if not m:
        return ""
    alter = m.group(2).count("#") - m.group(2).count("b")
    kind = _SUFFIX_KIND.get(m.group(3))
    kind_xml = '<kind text="%s">%s</kind>' % (escape(m.group(3)), kind) if kind else '<kind text="%s">other</kind>' % escape(m.group(3))
    bass = ""
    if m.group(4):
        ba = m.group(5).count("#") - m.group(5).count("b")
        bass = "<bass><bass-step>%s</bass-step>%s</bass>" % (m.group(4), "<bass-alter>%d</bass-alter>" % ba if ba else "")
    return "<harmony><root><root-step>%s</root-step>%s</root>%s%s</harmony>" % (m.group(1), "<root-alter>%d</root-alter>" % alter if alter else "", kind_xml, bass)


def score_to_musicxml(info):
    """MusicXML text of an import/transpose result dict."""
    return write_musicxml((info.get("melody") or {}).get("notes") or [], info.get("key_fifths") or 0, info.get("mode") or "major", info.get("time_signature"),
                          info.get("tempo"), info.get("pickup_qn") or 0.0, info.get("chords"), info.get("title"))


# -- the tool -------------------------------------------------------------------------------------------------------
def tool_transpose(args):
    semis, label = resolve_target(args.get("to"), args.get("octave", 0))
    if args.get("file") is not None:
        info = musicxml.import_musicxml(resolve_in(args["file"]), part=args.get("part"), expand_repeats=args.get("expand_repeats", True))
    elif isinstance(args.get("notes"), list):
        info = {"melody": {"notes": args["notes"]}, "chords": args.get("chords") or [], "key": args.get("key"), "mode": args.get("mode"), "key_fifths": args.get("key_fifths"),
                "time_signature": args.get("time_signature"), "tempo": args.get("tempo"), "pickup_qn": args.get("pickup_qn", 0.0), "title": args.get("title"), "warnings": []}
        for n in args["notes"]:
            if not isinstance(n, dict) or not isinstance(n.get("pitch"), int) or "start_qn" not in n or "length_qn" not in n:
                raise invalid("every note needs integer pitch, start_qn and length_qn", element="notes")
    else:
        raise invalid("give a file (MusicXML) or notes", element="file")
    out = transpose_score(info, semis)
    out["to"] = label
    out["semitones"] = semis
    if args.get("write_musicxml"):
        xml, w = score_to_musicxml(out)
        path = resolve_out(args["write_musicxml"], "transposed.musicxml")
        with open(path, "w", encoding="utf-8") as f:
            f.write(xml)
        out["musicxml_path"] = path
        out["warnings"] = out["warnings"] + w
    return out
