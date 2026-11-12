# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""A MusicXML reader (score-partwise, plain .xml and compressed .mxl) with the standard library only.

Output notes are exactly the note JSON of the core's midi.write / item.create_midi: {pitch (MIDI number), start_qn, length_qn}.
Bars are numbered as in a printed score: a pickup (anacrusis) is bar 0, the first full bar is bar 1; beats are 1-based and measured in the beat unit of the time signature.
"""
import io
import math
import re
import xml.etree.ElementTree as ET
import zipfile
from fractions import Fraction

from .errors import invalid

MAX_BYTES = 20 * 1024 * 1024
_STEP = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
_NAMES_SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
_NAMES_FLAT = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
_MODE_SHIFT = {"major": 0, "ionian": 0, "mixolydian": 1, "dorian": 2, "minor": 3, "aeolian": 3, "phrygian": 4, "locrian": 5, "lydian": -1}
_KIND = {"major": "", "minor": "m", "dominant": "7", "major-seventh": "maj7", "minor-seventh": "m7", "diminished": "dim", "augmented": "aug",
         "half-diminished": "m7b5", "diminished-seventh": "dim7", "suspended-fourth": "sus4", "suspended-second": "sus2", "major-sixth": "6",
         "minor-sixth": "m6", "dominant-ninth": "9", "major-ninth": "maj9", "minor-ninth": "m9", "dominant-11th": "11", "dominant-13th": "13",
         "power": "5", "minor-major": "mMaj7", "augmented-seventh": "7#5", "major-minor": "mMaj7"}
_BEAT_UNIT = {"whole": 4.0, "half": 2.0, "quarter": 1.0, "eighth": 0.5, "16th": 0.25, "32nd": 0.125}
_KK_MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
_KK_MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]


# -- music theory helpers shared with transpose.py ---------------------------------------------------------------
def spell_fifths(n):
    """Name of the pitch class at position n on the line of fifths (0 C, 1 G, ... 6 F#, -1 F, -2 Bb)."""
    k = (n + 1) // 7
    return "FCGDAEB"[(n + 1) % 7] + ("#" if k > 0 else "b") * abs(k)


def shift_fifths(fifths, semitones):
    """Key signature after moving up by semitones, choosing the enharmonic spelling with fewer accidentals."""
    f = (fifths + 7 * semitones) % 12
    return min([f, f - 12], key=lambda c: (abs(c), 0 if (c >= 0) == (fifths >= 0) else 1))


def pc_to_fifths(pc):
    f = (pc * 7) % 12
    return f - 12 if f > 6 else f


def key_name(fifths, mode):
    """Tonic name of a key signature + mode, e.g. (1, major) -> G, (1, minor) -> E."""
    return spell_fifths(fifths + _MODE_SHIFT.get(mode, 0))


def midi_name(midi, fifths=0):
    names = _NAMES_FLAT if fifths < 0 else _NAMES_SHARP
    return "%s%d" % (names[midi % 12], midi // 12 - 1)


def estimate_key(notes):
    """Krumhansl-Schmuckler key estimate from pitch-class durations. Returns {key, mode, fifths, confidence} or None without notes."""
    hist = [0.0] * 12
    for n in notes:
        hist[n["pitch"] % 12] += n["length_qn"]
    if sum(hist) <= 0:
        return None

    def corr(a, b):
        ma, mb = sum(a) / 12, sum(b) / 12
        num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
        den = math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))
        return num / den if den else 0.0

    scores = []
    for pc in range(12):
        rotated = hist[pc:] + hist[:pc]      # rotated[i] = duration at interval i above pc
        scores.append((corr(rotated, _KK_MAJOR), pc, "major"))
        scores.append((corr(rotated, _KK_MINOR), pc, "minor"))
    scores.sort(key=lambda s: (-s[0], s[1], s[2]))
    best, second = scores[0], scores[1]
    fifths = pc_to_fifths(best[1] if best[2] == "major" else (best[1] + 3) % 12)
    confidence = max(0.0, min(1.0, 0.5 * best[0] + 0.5 * min(1.0, (best[0] - second[0]) * 5)))
    return {"key": key_name(fifths, best[2]), "mode": best[2], "fifths": fifths, "confidence": round(confidence, 3)}


def chord_symbol(root_step, root_alter, kind, kind_text, bass_step=None, bass_alter=0):
    def name(step, alter):
        return step + ("#" * alter if alter > 0 else "b" * -alter)
    suffix = _KIND.get(kind)
    if suffix is None:
        suffix = kind_text or kind or ""
    out = name(root_step, root_alter) + suffix
    if bass_step:
        out += "/" + name(bass_step, bass_alter)
    return out


# -- reading the file -----------------------------------------------------------------------------------------------
def read_score_bytes(path):
    """The score XML of a .xml/.musicxml file or of a compressed .mxl (the rootfile named in META-INF/container.xml)."""
    try:
        with open(path, "rb") as f:
            data = f.read(MAX_BYTES + 1)
    except OSError as e:
        raise invalid("cannot read the file: %s" % e.strerror, file=str(path))
    if len(data) > MAX_BYTES:
        raise invalid("the file is larger than %d MB" % (MAX_BYTES // 1048576))
    if data[:2] == b"PK":
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                names = z.namelist()
                root = None
                if "META-INF/container.xml" in names:
                    container = ET.fromstring(z.read("META-INF/container.xml"))
                    for el in container.iter():
                        if el.tag.split("}")[-1] == "rootfile" and el.get("full-path"):
                            root = el.get("full-path")
                            break
                if root is None:
                    root = next((n for n in names if n.lower().endswith((".xml", ".musicxml")) and not n.startswith("META-INF/")), None)
                if root is None or root not in names:
                    raise invalid("the .mxl archive has no MusicXML rootfile")
                if z.getinfo(root).file_size > MAX_BYTES:
                    raise invalid("the score inside the .mxl is larger than %d MB" % (MAX_BYTES // 1048576))
                with z.open(root) as f:
                    return f.read(MAX_BYTES + 1)
        except (zipfile.BadZipFile, ET.ParseError, KeyError, RuntimeError, NotImplementedError) as e:
            raise invalid("the .mxl archive is damaged: %s" % e)
    return data


def _local(tag):
    return tag.split("}")[-1] if isinstance(tag, str) else ""


def _child(el, name):
    for c in el:
        if _local(c.tag) == name:
            return c
    return None


def _children(el, name):
    return [c for c in el if _local(c.tag) == name]


def _text(el, name, default=None):
    c = _child(el, name)
    return c.text.strip() if c is not None and c.text is not None else default


def _int(el, name, measure=None, default=None, required=False):
    t = _text(el, name)
    if t is None:
        if required:
            raise invalid("<%s> is missing inside <%s>" % (name, _local(el.tag)), element=name, measure=measure)
        return default
    try:
        return int(t)
    except ValueError:
        try:
            f = float(t)
        except ValueError:
            raise invalid("<%s> is not a number: %r" % (name, t), element=name, measure=measure)
        if f != int(f):
            raise invalid("<%s> must be an integer: %r" % (name, t), element=name, measure=measure)
        return int(f)


def parse_xml(data):
    if b"<!ENTITY" in data:
        raise invalid("XML entity declarations are not accepted")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as e:
        line, col = getattr(e, "position", (None, None))
        raise invalid("malformed XML: %s" % e, line=line, column=col)
    except (ValueError, UnicodeDecodeError) as e:
        raise invalid("malformed XML: %s" % e)
    name = _local(root.tag)
    if name == "score-timewise":
        raise invalid("score-timewise is not supported, export score-partwise", element=name)
    if name != "score-partwise":
        raise invalid("not a MusicXML score: the root element is <%s>" % name, element=name)
    return root


# -- one part -> measures -------------------------------------------------------------------------------------------
def _pitch(note, measure):
    p = _child(note, "pitch")
    step = _text(p, "step")
    if step not in _STEP:
        raise invalid("<step> must be one of A-G", element="step", measure=measure)
    alter = _text(p, "alter", "0")
    try:
        alter = int(round(float(alter)))
    except ValueError:
        raise invalid("<alter> is not a number: %r" % alter, element="alter", measure=measure)
    octave = _int(p, "octave", measure, required=True)
    return (octave + 1) * 12 + _STEP[step] + alter


def _metronome_bpm(metronome):
    unit = _text(metronome, "beat-unit")
    per = _text(metronome, "per-minute")
    if unit not in _BEAT_UNIT or per is None:
        return None
    try:
        bpm = float(per.split("-")[0])
    except ValueError:
        return None
    return bpm * _BEAT_UNIT[unit] * (1.5 if _child(metronome, "beat-unit-dot") is not None else 1.0)


def _read_measures(part):
    measures = []
    divisions, time, transpose, key = 1, (4, 4), 0, None
    for mel in _children(part, "measure"):
        number = mel.get("number", str(len(measures) + 1))
        m = {"number": number, "implicit": mel.get("implicit") == "yes", "notes": [], "harmonies": [], "tempos": [], "end": 0, "forward": False,
             "backward": 0, "ending": None, "ending_start": False, "ending_stop": False, "ending_nums": None, "key": None}
        cursor, last_start, end = 0, 0, 0
        for el in mel:
            tag = _local(el.tag)
            if tag == "attributes":
                d = _int(el, "divisions", number)
                if d is not None:
                    if d <= 0:
                        raise invalid("<divisions> must be positive", element="divisions", measure=number)
                    divisions = d
                k = _child(el, "key")
                if k is not None and _child(k, "fifths") is not None:
                    key = (_int(k, "fifths", number), _text(k, "mode", "major").lower())
                    m["key"] = key
                t = _child(el, "time")
                if t is not None and _child(t, "beats") is not None:
                    try:
                        beats = sum(int(x) for x in _text(t, "beats").split("+"))
                    except ValueError:
                        raise invalid("<beats> is not a number", element="beats", measure=number)
                    beat_type = _int(t, "beat-type", number, required=True)
                    if beats <= 0 or beat_type <= 0:
                        raise invalid("the time signature must be positive", element="time", measure=number)
                    time = (beats, beat_type)
                tr = _child(el, "transpose")
                if tr is not None:
                    transpose = _int(tr, "chromatic", number, 0) + 12 * _int(tr, "octave-change", number, 0)
            elif tag == "note":
                if _child(el, "grace") is not None or _child(el, "cue") is not None:
                    continue
                is_chord = _child(el, "chord") is not None
                dur = _int(el, "duration", number, required=True)
                if dur < 0:
                    raise invalid("<duration> must not be negative", element="duration", measure=number)
                start = last_start if is_chord else cursor
                if _child(el, "rest") is None and _child(el, "unpitched") is None:
                    if _child(el, "pitch") is None:
                        raise invalid("a <note> needs <pitch>, <rest> or <unpitched>", element="note", measure=number)
                    ties = {t.get("type") for t in _children(el, "tie")}
                    notations = _child(el, "notations")
                    if not ties and notations is not None:
                        ties = {t.get("type") for t in _children(notations, "tied")}
                    m["notes"].append({"pitch": _pitch(el, number), "start": start, "dur": dur, "voice": _text(el, "voice", "1"), "staff": _text(el, "staff", "1"),
                                       "tie_start": "start" in ties, "tie_stop": "stop" in ties, "div": divisions, "transpose": transpose})
                if not is_chord:
                    last_start = cursor
                    cursor += dur
                end = max(end, start + dur)
            elif tag == "backup":
                cursor -= _int(el, "duration", number, required=True)
                if cursor < 0:
                    raise invalid("<backup> goes before the start of the measure", element="backup", measure=number)
            elif tag == "forward":
                cursor += _int(el, "duration", number, required=True)
                end = max(end, cursor)
            elif tag == "harmony":
                root = _child(el, "root")
                if root is None or _text(root, "root-step") not in _STEP:
                    continue
                kind = _child(el, "kind")
                bass = _child(el, "bass")
                try:
                    ra = int(round(float(_text(root, "root-alter", "0"))))
                    ba = int(round(float(_text(bass, "bass-alter", "0")))) if bass is not None else 0
                except ValueError:
                    raise invalid("<root-alter> is not a number", element="harmony", measure=number)
                sym = chord_symbol(_text(root, "root-step"), ra, (kind.text or "").strip() if kind is not None else "", kind.get("text") if kind is not None else None,
                                   _text(bass, "bass-step") if bass is not None else None, ba)
                m["harmonies"].append({"pos": cursor + _int(el, "offset", number, 0), "div": divisions, "symbol": sym})
            elif tag == "direction" or (tag == "sound" and el.get("tempo")):
                sound = el if tag == "sound" else _child(el, "sound")
                bpm = None
                if sound is not None and sound.get("tempo"):
                    try:
                        bpm = float(sound.get("tempo"))
                    except ValueError:
                        raise invalid("the tempo of <sound> is not a number", element="sound", measure=number)
                elif tag == "direction":
                    for dt in _children(el, "direction-type"):
                        met = _child(dt, "metronome")
                        if met is not None:
                            bpm = _metronome_bpm(met)
                if bpm and bpm > 0:
                    off = _int(el, "offset", number, 0) if tag == "direction" else 0
                    m["tempos"].append({"pos": cursor + off, "div": divisions, "bpm": bpm})
            elif tag == "barline":
                rep = _child(el, "repeat")
                if rep is not None:
                    if rep.get("direction") == "forward":
                        m["forward"] = True
                    elif rep.get("direction") == "backward":
                        try:
                            m["backward"] = max(1, int(rep.get("times", "2")) - 1)
                        except ValueError:
                            m["backward"] = 1
                end_el = _child(el, "ending")
                if end_el is not None:
                    if end_el.get("type") == "start":
                        m["ending_start"] = True
                        m["ending_nums"] = {int(x) for x in re.findall(r"\d+", end_el.get("number", ""))}
                    elif end_el.get("type") in ("stop", "discontinue"):
                        m["ending_stop"] = True
        m["end"] = end
        m["div"] = divisions
        m["time"] = time
        m["transpose"] = transpose
        measures.append(m)
    # measures between an ending start and its stop/discontinue carry the numbers of that ending
    current = None
    for m in measures:
        if m["ending_start"]:
            current = m["ending_nums"]
        m["ending"] = current
        m["ending_end"] = m["ending_stop"] and current is not None
        if m["ending_end"]:
            current = None
    return measures


def _play_order(measures, expand):
    n = len(measures)
    if not expand:
        return list(range(n))
    order, i, repeat_start, pass_no, done, steps = [], 0, 0, 1, {}, 0
    while i < n:
        steps += 1
        if steps > 20 * n + 1000:
            raise invalid("the repeat structure does not terminate")
        m = measures[i]
        if m["ending"] and pass_no not in m["ending"]:
            i += 1
            continue
        if m["forward"]:
            repeat_start = i
        order.append(i)
        if m["backward"] and done.get(i, 0) < m["backward"]:
            done[i] = done.get(i, 0) + 1
            pass_no += 1
            i = repeat_start
            continue
        if m["backward"] or m["ending_end"]:
            pass_no, repeat_start = 1, i + 1
            done.pop(i, None)
        i += 1
    return order


def parse_score(data, part=None, expand_repeats=True, concert=True, voice=None):
    """Parses score XML bytes. `concert` applies <transpose> of transposing instruments (sounding pitch); `voice` keeps one voice number."""
    root = parse_xml(data)
    warnings = []
    plist = _child(root, "part-list")
    names = {}
    if plist is not None:
        for sp in _children(plist, "score-part"):
            prog = _child(sp, "midi-instrument")
            names[sp.get("id")] = {"name": _text(sp, "part-name", ""), "midi_program": _int(prog, "midi-program") if prog is not None else None}
    parts = _children(root, "part")
    if not parts:
        raise invalid("the score has no <part>", element="part")
    infos = []
    for idx, p in enumerate(parts, 1):
        info = names.get(p.get("id"), {})
        infos.append({"index": idx, "id": p.get("id"), "name": info.get("name", ""), "midi_program": info.get("midi_program")})
    chosen = 0
    if part is not None:
        sel = str(part)
        found = [i for i, inf in enumerate(infos) if sel == inf["id"] or sel == inf["name"]]
        if not found and sel.isdigit() and 1 <= int(sel) <= len(infos):
            found = [int(sel) - 1]
        if not found:
            raise invalid("no part %r; parts: %s" % (part, ", ".join("%s (%s)" % (i["id"], i["name"]) for i in infos)), element="part")
        chosen = found[0]
    measures = _read_measures(parts[chosen])
    if not measures:
        raise invalid("the part has no measures", element="measure")

    first = measures[0]
    first_len = Fraction(first["end"], first["div"])
    first_bar = Fraction(first["time"][0] * 4, first["time"][1])
    pickup = first_len > 0 and (first["implicit"] or first["number"] == "0" or (first_len < first_bar and len(measures) > 1))
    order = _play_order(measures, expand_repeats)

    events, chords, tempos, time_sigs = [], [], [], []
    t = Fraction(0)
    for pos, idx in enumerate(order):
        m = measures[idx]
        beats, beat_type = m["time"]
        bar_len = Fraction(beats * 4, beat_type)
        content = Fraction(m["end"], m["div"])
        mlen = content if ((idx == 0 and pickup) or m["implicit"] or content > bar_len) else bar_len
        bar = pos if pickup else pos + 1
        unit = Fraction(4, beat_type)
        if not time_sigs or time_sigs[-1] != (beats, beat_type):
            time_sigs.append((beats, beat_type))
        for n in m["notes"]:
            pitch = n["pitch"] + (n["transpose"] if concert else 0)
            if not 0 <= pitch <= 127:
                warnings.append("bar %d: a note outside the MIDI range was dropped" % bar)
                continue
            events.append((t + Fraction(n["start"], n["div"]), Fraction(n["dur"], n["div"]), pitch, n))
        for h in m["harmonies"]:
            chords.append({"bar": bar, "beat": round(float(1 + Fraction(h["pos"], h["div"]) / unit), 4), "symbol": h["symbol"]})
        for tp in m["tempos"]:
            tempos.append({"bar": bar, "beat": round(float(1 + Fraction(tp["pos"], tp["div"]) / unit), 4), "bpm": round(tp["bpm"], 3)})
        t += mlen

    events.sort(key=lambda e: (e[0], e[2]))
    notes, open_ties = [], {}
    for start, length, pitch, n in events:
        if voice is not None and n["voice"] != str(voice):
            continue
        key = (pitch, n["voice"], n["staff"])
        prev = open_ties.get(key)
        if n["tie_stop"]:
            if prev is not None and prev["start"] + prev["length"] == start:
                prev["length"] += length
                if not n["tie_start"]:
                    del open_ties[key]
                continue
            warnings.append("a tie to %s has no matching start" % midi_name(pitch))
        item = {"start": start, "length": length, "pitch": pitch}
        notes.append(item)
        if n["tie_start"]:
            open_ties[key] = item
        else:
            open_ties.pop(key, None)
    out_notes = [{"pitch": n["pitch"], "start_qn": round(float(n["start"]), 6), "length_qn": round(float(n["length"]), 6)} for n in notes if n["length"] > 0]

    sig = next((m["key"] for m in measures if m["key"] is not None), None)
    shift = measures[0]["transpose"] if concert else 0
    if sig is not None:
        fifths, mode = sig
        if shift:
            fifths = shift_fifths(fifths, shift)
        keyinfo = {"key": key_name(fifths, mode), "mode": mode, "key_fifths": fifths, "key_confidence": 1.0, "key_source": "signature"}
    else:
        est = estimate_key(out_notes)
        if est:
            keyinfo = {"key": est["key"], "mode": est["mode"], "key_fifths": est["fifths"], "key_confidence": est["confidence"], "key_source": "estimated"}
            warnings.append("no key signature in the file: the key is estimated (confidence %.2f)" % est["confidence"])
        else:
            keyinfo = {"key": None, "mode": None, "key_fifths": None, "key_confidence": 0.0, "key_source": "none"}
    if len(time_sigs) > 1:
        warnings.append("the time signature changes (%s)" % ", ".join("%d/%d" % s for s in time_sigs))
    if not out_notes:
        warnings.append("the selected part has no notes")
    title = None
    work = _child(root, "work")
    if work is not None:
        title = _text(work, "work-title")
    if not title:
        title = _text(root, "movement-title")
    result = {"format": "musicxml", "title": title or None, "parts": infos, "part": infos[chosen]["id"]}
    result.update(keyinfo)
    result.update({"time_signature": {"beats": time_sigs[0][0], "beat_type": time_sigs[0][1]}, "tempo": tempos[0]["bpm"] if tempos else None, "tempo_changes": tempos,
                   "pickup_qn": round(float(first_len), 6) if pickup else 0.0, "bars": len(order) - (1 if pickup else 0), "length_qn": round(float(t), 6),
                   "expanded_repeats": bool(expand_repeats), "transposed_semitones": shift,
                   "melody": {"notes": out_notes}, "chords": chords, "warnings": warnings})
    return result


def import_musicxml(path, **kw):
    return parse_score(read_score_bytes(path), **kw)
