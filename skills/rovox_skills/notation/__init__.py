# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Notation tools of the sidecar: notation.import, notation.transpose, notation.engrave (standard library; LilyPond, abc2midi and verovio are optional and reported as SKILL_UNAVAILABLE)."""
import os

from ..core import CoreError
from . import abc, engrave, musicxml, transpose
from .errors import NotationError, invalid
from .paths import resolve_in

_STR = {"type": "string"}
_BOOL = {"type": "boolean"}
_INT = {"type": "integer"}
_NOTE = {"type": "object", "description": "A note as the core's midi.write accepts it.",
         "properties": {"pitch": {"type": "integer", "description": "MIDI note number 0..127 (60 = C4)"}, "start_qn": {"type": "number", "description": "Start in quarter notes from the start of the item, 0-based"},
                        "length_qn": {"type": "number", "description": "Length in quarter notes"}},
         "required": ["pitch", "start_qn", "length_qn"]}


def _import(core_factory, args):
    path = resolve_in(args.get("file"))
    fmt = args.get("format") or ("abc" if path.lower().endswith(".abc") else "musicxml")
    if fmt not in ("musicxml", "mxl", "abc"):
        raise invalid("format must be musicxml, mxl or abc", element="format")
    if fmt == "abc":
        info = abc.import_abc(path)
    else:
        info = musicxml.import_musicxml(path, part=args.get("part"), expand_repeats=args.get("expand_repeats", True), concert=args.get("concert_pitch", True),
                                        voice=args.get("voice"))
    target = args.get("write_to")
    if target is not None:
        if not isinstance(target, dict) or not target.get("project_id") or not target.get("track_id"):
            raise invalid("write_to needs project_id and track_id", element="write_to")
        notes = info["melody"]["notes"]
        if not notes:
            raise invalid("there are no notes to write", element="write_to")
        core = core_factory("skill:notation.import")
        name = target.get("name") or info.get("title") or os.path.basename(path)
        item = core.call("item.create_midi", project_id=target["project_id"], track_id=target["track_id"], start=target.get("start") or {"bar": 1}, name=name, notes=notes,
                         undo_label="skill notation.import")
        info["written"] = {"item_id": item.get("item_id"), "notes": len(notes)}
    return info


def _transpose(core_factory, args):
    return transpose.tool_transpose(args)


def _engrave(core_factory, args):
    return engrave.tool_engrave(args)


NOTATION_TOOLS = {
    "notation.import": {
        "handler": _import,
        "description": "Reads a score (MusicXML .xml/.musicxml, compressed .mxl, or ABC through abc2midi) and returns the melody as notes in the JSON of midi.write "
                       "({pitch: MIDI number, start_qn, length_qn} in quarter notes from 0), plus key/mode with key_confidence (estimated when the file has no key signature), "
                       "time_signature, tempo (quarter notes per minute), chords [{bar, beat, symbol}] (bars 1-based, a pickup is bar 0, beats 1-based), pickup_qn, parts and warnings. "
                       "Repeats and first/second endings are expanded unless expand_repeats is false; ties are merged; transposing instruments are returned at concert pitch. "
                       "With write_to the notes are also written as a new MIDI item on a track of the open project.",
        "inputSchema": {"type": "object", "properties": {
            "file": {"type": "string", "description": "Path of the score file inside the directories the sidecar may read"},
            "format": {"type": "string", "enum": ["musicxml", "mxl", "abc"], "description": "Defaults from the extension"},
            "part": {"type": "string", "description": "Part id, part name or 1-based index; default the first part"},
            "expand_repeats": {"type": "boolean", "description": "Play repeats and endings out (default true)"},
            "voice": {"type": "string", "description": "Keep only this MusicXML voice number"},
            "concert_pitch": {"type": "boolean", "description": "Apply the instrument transposition so notes sound as played (default true)"},
            "write_to": {"type": "object", "description": "Also create a MIDI item with the notes",
                         "properties": {"project_id": _STR, "track_id": _STR, "name": _STR, "start": {"type": "object", "description": "Core time position, default {bar: 1}"}},
                         "required": ["project_id", "track_id"]}}, "required": ["file"]}},
    "notation.transpose": {
        "handler": _transpose,
        "description": "Transposes a score for a transposing instrument or by semitones. to is C, Bb (clarinet, trumpet, soprano sax: written a major 2nd higher), Eb (alto sax: major 6th higher), "
                       "F (horn: perfect 5th higher), piccolo/soprano_recorder (octave lower written), or an integer of semitones added to the concert pitch; octave adds whole octaves. "
                       "Returns the transposed notes (midi.write JSON), the new key (the enharmonic spelling with fewer accidentals), chords transposed and range warnings. "
                       "Give a MusicXML file, or notes from notation.import. write_musicxml saves a transposed one-part MusicXML for engraving.",
        "inputSchema": {"type": "object", "properties": {
            "file": {"type": "string", "description": "MusicXML/mxl path inside the directories the sidecar may read"},
            "notes": {"type": "array", "description": "Alternative to file: notes of notation.import", "items": _NOTE},
            "key_fifths": {"type": "integer", "description": "With notes: key signature, -7..7 (flats negative)"},
            "key": {"type": "string", "description": "With notes: tonic such as G or Bb when key_fifths is not given"},
            "mode": {"type": "string", "description": "With notes: major or minor"},
            "chords": {"type": "array", "description": "With notes: [{bar, beat, symbol}]", "items": {"type": "object"}},
            "time_signature": {"type": "object", "description": "With notes: {beats, beat_type}"},
            "tempo": {"type": "number", "description": "With notes: quarter notes per minute"},
            "pickup_qn": {"type": "number", "description": "With notes: length of the pickup bar in quarter notes"},
            "to": {"anyOf": [{"type": "string"}, {"type": "integer"}], "description": "Target instrument (C, Bb, Eb, F, alto_sax, ...) or semitones"},
            "octave": {"type": "integer", "description": "Extra octaves, -4..4"},
            "part": {"type": "string", "description": "Part id, name or 1-based index"},
            "expand_repeats": {"type": "boolean", "description": "Default true"},
            "write_musicxml": {"type": "string", "description": "Path (inside an allowed directory) for the transposed MusicXML"}}, "required": ["to"]}},
    "notation.engrave": {
        "handler": _engrave,
        "description": "Engraves a MusicXML score to PDF or SVG with LilyPond (musicxml2ly + lilypond; needs the skills-notation image) or SVG with verovio (optional extra notation). "
                       "transpose_for (C, Bb, Eb, F or semitones) engraves the written part for that instrument. The file is written inside an allowed directory; "
                       "returns {path, bytes, pages}. A missing engine answers SKILL_UNAVAILABLE.",
        "inputSchema": {"type": "object", "properties": {
            "file": {"type": "string", "description": "MusicXML/mxl path inside the directories the sidecar may read"},
            "engine": {"type": "string", "enum": ["lilypond", "verovio"], "description": "Default lilypond for pdf, verovio for svg when installed"},
            "format": {"type": "string", "enum": ["pdf", "svg"], "description": "Default pdf"},
            "transpose_for": {"anyOf": [{"type": "string"}, {"type": "integer"}], "description": "Instrument or semitones, as in notation.transpose"},
            "octave": {"type": "integer", "description": "Extra octaves with transpose_for"},
            "part": {"type": "string", "description": "Part id, name or 1-based index (with transpose_for)"},
            "output": {"type": "string", "description": "Output path inside an allowed directory; default <file name>.<format> in the output directory"}}, "required": ["file"]}},
}


def tool_specs():
    """The MCP tool descriptors (name, description, inputSchema) for tools/list."""
    return [{"name": n, "description": t["description"], "inputSchema": t["inputSchema"]} for n, t in NOTATION_TOOLS.items()]


def call_tool(name, args, core_factory):
    """Runs a notation tool. Returns (True, data) or (False, {code, message, details?})."""
    try:
        if not isinstance(args, dict):
            raise invalid("arguments must be an object")
        return True, NOTATION_TOOLS[name]["handler"](core_factory, args)
    except NotationError as e:
        return False, e.payload()
    except CoreError as e:
        return False, {"code": e.code, "message": str(e), "details": e.details}
    except (KeyError, TypeError, ValueError, AttributeError) as e:
        return False, {"code": "INVALID_PARAMS", "message": "bad arguments: %s: %s" % (type(e).__name__, e)}
    except (RuntimeError, OSError) as e:
        return False, {"code": "SKILL_FAILED", "message": str(e)}
