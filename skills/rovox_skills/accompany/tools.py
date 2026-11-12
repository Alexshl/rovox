# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""MCP tools of the accompany skill: accompany.styles, accompany.generate, accompany.apply. `wrap_handler` adds them to the sidecar's stdio server."""
import json

from ..core import CoreError
from .generate import generate, style_catalog

_NOTE = {"type": "object", "description": "A melody note.", "properties": {
    "pitch": {"type": ["integer", "string"], "description": "MIDI note number (60 = C4) or a name such as G4 or Bb3."},
    "start_qn": {"type": "number", "description": "Start in quarter notes from the first bar of the piece (0 = bar 1, beat 1)."},
    "length_qn": {"type": "number", "description": "Length in quarter notes."}}, "required": ["pitch", "start_qn", "length_qn"]}
_CHORD = {"type": "object", "description": "A chord change.", "properties": {
    "bar": {"type": "integer", "description": "1-based bar number."},
    "beat": {"type": "number", "description": "1-based beat in the unit of the time signature's denominator (default 1)."},
    "symbol": {"type": "string", "description": "Chord symbol: C, Am, G7, Dm7, F/A, Bb, C#dim, Esus4, Cmaj7, Bm7b5, Cadd9, C6, C9. Unknown extensions are played as the triad with a warning."}},
    "required": ["bar", "symbol"]}


def _props():
    return {
        "melody": {"type": "array", "description": "The melody the accompaniment goes with (optional). The comp is kept below its lowest note, and without chords the melody is harmonised.", "items": _NOTE},
        "chords": {"type": "array", "description": "Chord changes, 1-based {bar, beat, symbol}. Omit to let a basic harmoniser (I, IV, V, vi per bar) choose them from the melody; the chosen chords are returned so they can be edited and passed back.", "items": _CHORD},
        "key": {"type": "string", "description": "Key such as G, Bb or Em. Needed when chords are omitted."},
        "time_signature": {"type": "string", "description": "3/4, 4/4, 2/4 or 6/8 (what the chosen style supports; see accompany.styles). Default 4/4."},
        "tempo": {"type": "number", "description": "Beats per minute (quarter notes). Default 100."},
        "bars": {"type": "integer", "description": "Number of bars of the body; measured from the melody or chords when omitted."},
        "style": {"type": "string", "description": "Style id from accompany.styles: waltz, ballad, pop-8th, march. Default pop-8th."},
        "level": {"type": "string", "description": "beginner (sparse, steady, root-fifth bass, block chords on the beat) or intermediate (fuller rhythm, passing notes, fills). Default beginner."},
        "seed": {"type": "integer", "description": "The same inputs and seed give identical output; another seed changes only the variations (bar variants, fills, velocities). Default 0."},
        "parts": {"type": "array", "description": "Which parts to write, a subset of bass, comp, drums. Default: all the style defines at this level.", "items": {"type": "string", "description": "bass, comp or drums."}},
        "count_in_bars": {"type": "integer", "description": "Bars of click before the music (0-8, default 0); the notes are shifted by them."},
        "intro_bars": {"type": "integer", "description": "Bars of intro on the first chord before the melody starts (0-8, default 0)."},
        "ending": {"type": "boolean", "description": "Make the last bar a sustained final chord (default true). A ritardando is left to the renderer."},
    }


def _apply_props():
    props = _props()
    props["project_id"] = {"type": "string", "description": "The Rovox project to write into."}
    props["write"] = {"type": "boolean", "description": "true (default) writes to the project; false only returns what would be written."}
    props["set_tempo"] = {"type": "boolean", "description": "Also set the project tempo to `tempo` with tempo.set (default false)."}
    props["name_prefix"] = {"type": "string", "description": "Prefix for the new track names, e.g. Waltz (default none)."}
    return props


ACCOMPANY_TOOLS = [
    {"name": "accompany.styles", "description": "Lists the accompaniment style packs with their id, version, meters, tempo range, levels and parts.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "accompany.generate", "description": "Generates a deterministic backing (bass, comp, drums) for a melody or a chord chart in a style at a level, as note lists in quarter notes. Nothing is written to a project; use accompany.apply for that. Returns tracks, the chords used (edit and pass them back), warnings and the timeline (count-in and intro shift the notes; body_start_qn is where bar 1 of the music begins).",
     "inputSchema": {"type": "object", "properties": _props()}},
    {"name": "accompany.apply", "description": "Generates the accompaniment (same parameters as accompany.generate) and, with write true, creates one track per part in the Rovox project with a MIDI item at bar 1 (drums get the built-in rovox:drums sampler; bass and comp tracks need an instrument added afterwards), optionally setting the tempo. The project's time signature should match. Returns the created track and item ids with the generated chords and warnings.",
     "inputSchema": {"type": "object", "properties": _apply_props(), "required": ["project_id"]}},
]

_GENERATE_KEYS = ("melody", "chords", "key", "time_signature", "tempo", "bars", "style", "level", "seed", "parts", "count_in_bars", "intro_bars", "ending")


def run_generate(args):
    return generate(**{k: args[k] for k in _GENERATE_KEYS if args.get(k) is not None})


def apply(core, args):
    """Creates the tracks and MIDI items in a project: per part one track.create and one item.create_midi (plus fx.add for the drum sampler), one tempo.set if asked."""
    if not args.get("project_id"):
        raise ValueError("project_id is required")
    result = run_generate(args)
    summary = {k: result[k] for k in ("chords", "style", "level", "seed", "timeline", "analysis", "warnings")}
    if args.get("write", True) is False:
        summary["written"] = False
        summary["tracks"] = result["tracks"]
        return summary
    pid, prefix = args["project_id"], args.get("name_prefix") or ""
    length = result["timeline"]["total_qn"]
    created = []
    if args.get("set_tempo"):
        core.call("tempo.set", project_id=pid, points=[{"bpm": float(result["tempo"]), "qn": 0}])
    for t in result["tracks"]:
        name = (prefix + " " if prefix else "") + t["name"]
        track_id = core.call("track.create", project_id=pid, name=name)["track_id"]
        instrument = None
        if t["role"] in ("drums", "click"):
            core.call("fx.add", project_id=pid, track_id=track_id, builtin="rovox:drums")
            instrument = "rovox:drums"
        item = core.call("item.create_midi", project_id=pid, track_id=track_id, start={"bar": 1}, length={"qn": length},
                         name="%s (%s, %s, seed %s)" % (name, result["style"]["id"], result["level"], result["seed"]), notes=t["notes"], undo_label="skill accompany: " + name)
        created.append({"role": t["role"], "name": name, "track_id": track_id, "item_id": item["item_id"], "notes": len(t["notes"]), "instrument": instrument})
    summary["written"] = True
    summary["tracks"] = created
    summary["tempo_set"] = bool(args.get("set_tempo"))
    return summary


def _text(ok, data=None, error=None):
    envelope = {"ok": ok}
    envelope["data" if ok else "error"] = data if ok else error
    return {"content": [{"type": "text", "text": json.dumps(envelope)}], "structuredContent": envelope, "isError": not ok}


def wrap_handler(inner):
    """Wraps the server's _handle(message, core_factory): accompany.* tool calls are answered here, everything else goes to the inner handler."""
    def handle(message, core_factory):
        if isinstance(message, dict) and message.get("method") == "tools/call" and message.get("id") is not None:
            p = message.get("params", {})
            name = p.get("name", "")
            if name in ("accompany.styles", "accompany.generate", "accompany.apply"):
                args = p.get("arguments", {}) or {}
                try:
                    if name == "accompany.styles":
                        data = {"styles": style_catalog()}
                    elif name == "accompany.generate":
                        data = run_generate(args)
                    else:
                        data = apply(core_factory("skill:accompany"), args)
                    out = _text(True, data)
                except CoreError as e:
                    out = _text(False, error={"code": e.code, "message": str(e), "details": e.details})
                except (KeyError, ValueError, TypeError, RuntimeError, OSError) as e:
                    out = _text(False, error={"code": "SKILL_FAILED", "message": "%s: %s" % (type(e).__name__, e)})
                return {"jsonrpc": "2.0", "id": message["id"], "result": out}
        return inner(message, core_factory)
    return handle
