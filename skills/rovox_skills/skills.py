# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""The skills. Each one is a function (core, **params) -> dict that only calls tools of the core, so everything it does is in the project's
activity log with the origin skill:<name> and is undoable. They are deterministic: the same params and seed give the same result."""
import random

REGISTRY = {}


def skill(name, description, params):
    def wrap(fn):
        REGISTRY[name] = {"name": name, "description": description, "params": params, "fn": fn}
        return fn
    return wrap


def _need(params, *names):
    missing = [n for n in names if params.get(n) in (None, "")]
    if missing:
        raise ValueError("missing parameter(s): " + ", ".join(missing))


# -- drum and bass patterns (pure functions, tested without a core) -----------------------------------------------------
KICK, SNARE, HAT_CLOSED, HAT_OPEN = 36, 38, 42, 46


def drum_pattern(bars=2, seed=0, swing=0.0, beats_per_bar=4):
    """Notes {pitch, start_qn, length_qn, velocity} of a rock/pop groove with seeded ghost notes and open-hat variations."""
    rng = random.Random(seed)
    notes = []
    for bar in range(bars):
        base = bar * beats_per_bar
        for step in range(beats_per_bar * 2):                       # eighth notes
            t = base + step * 0.5 + (swing * 0.25 if step % 2 else 0.0)
            accent = 100 if step % 2 == 0 else 72
            hat = HAT_OPEN if (step == beats_per_bar * 2 - 1 and rng.random() < 0.4) else HAT_CLOSED
            notes.append({"pitch": hat, "start_qn": round(t, 4), "length_qn": 0.25, "velocity": accent + rng.randint(-6, 6)})
        for beat in range(beats_per_bar):
            if beat % 2 == 0:
                notes.append({"pitch": KICK, "start_qn": base + beat, "length_qn": 0.25, "velocity": 112 + rng.randint(-4, 4)})
            else:
                notes.append({"pitch": SNARE, "start_qn": base + beat, "length_qn": 0.25, "velocity": 108 + rng.randint(-4, 4)})
        if rng.random() < 0.5:                                      # an extra kick before the next bar
            notes.append({"pitch": KICK, "start_qn": base + beats_per_bar - 0.5, "length_qn": 0.25, "velocity": 96})
    return sorted(notes, key=lambda n: (n["start_qn"], n["pitch"]))


def bass_pattern(root=36, bars=2, seed=0, scale=(0, 3, 5, 7, 10), beats_per_bar=4):
    """A root-heavy eighth-note bass line in a minor pentatonic scale."""
    rng = random.Random(seed + 1)
    notes = []
    for bar in range(bars):
        for step in range(beats_per_bar * 2):
            t = bar * beats_per_bar + step * 0.5
            if step % 4 == 0:
                pitch = root
            elif rng.random() < 0.35:
                continue
            else:
                pitch = root + scale[rng.randrange(len(scale))]
            notes.append({"pitch": pitch, "start_qn": t, "length_qn": 0.45, "velocity": 96 + rng.randint(-8, 8)})
    return notes


def _track_by_name(core, project_id, name):
    for t in core.call("track.list", project_id=project_id)["tracks"]:
        if t["name"].lower() == name.lower():
            return t
    return None


def _find_instrument(core, query):
    res = core.call("plugin.catalog", kind="instrument", query=query, status="ok", limit=1)
    return res["plugins"][0] if res["plugins"] else None


@skill("rhythm_section", "Writes a seeded drum and bass pattern as MIDI on the project's instrument tracks (creating the tracks and loading instruments from the catalog if needed).",
       {"project_id": "string", "bars": "int (default 2)", "seed": "int (default 0)", "drums": "plugin query (default 'drum')", "bass": "plugin query (default 'zynaddsubfx')",
        "root": "MIDI note of the bass root (default 36)"})
def rhythm_section(core, project_id=None, bars=2, seed=0, drums="drum", bass="zynaddsubfx", root=36, **_):
    _need({"project_id": project_id}, "project_id")
    report = {"tracks": {}, "items": {}}
    for role, query, notes in (("Drums", drums, drum_pattern(int(bars), int(seed))), ("Bass", bass, bass_pattern(int(root), int(bars), int(seed)))):
        track = _track_by_name(core, project_id, role)
        if track is None:
            plugin = _find_instrument(core, query)
            if plugin is None:
                raise ValueError("no installed instrument matches '%s' for the %s track (plugin.catalog lists what is available)" % (query, role))
            track_id = core.call("track.create", project_id=project_id, name=role)["track_id"]
            core.call("fx.add", project_id=project_id, track_id=track_id, plugin={"plugin_uid": plugin["plugin_uid"]})
            report["tracks"][role] = {"track_id": track_id, "instrument": plugin["name"]}
        else:
            track_id = track["track_id"]
            report["tracks"][role] = {"track_id": track_id, "instrument": "existing"}
        item = core.call("item.create_midi", project_id=project_id, track_id=track_id, start={"bar": 1}, name="%s (seed %s)" % (role, seed), notes=notes,
                         undo_label="skill rhythm_section: %s" % role)
        report["items"][role] = {"item_id": item["item_id"], "notes": len(notes)}
    return report


@skill("add_reverb_bus", "Creates a reverb return track and sends the given tracks to it.", {"project_id": "string", "track_ids": "list of track ids", "send_db": "number (default -12)"})
def add_reverb_bus(core, project_id=None, track_ids=None, send_db=-12.0, **_):
    _need({"project_id": project_id}, "project_id")
    bus = core.call("track.create", project_id=project_id, name="Reverb")["track_id"]
    core.call("fx.add", project_id=project_id, track_id=bus, builtin="reverb")
    sends = []
    for tid in track_ids or []:
        sends.append(core.call("routing.send_create", project_id=project_id, track_id=tid, to_track_id=bus, gain_db=float(send_db))["send_id"])
    return {"bus_track_id": bus, "sends": sends}


@skill("render_and_deliver", "Renders the master at a loudness target and delivers it with an adapter (respects the project's delivery hold).",
       {"project_id": "string", "adapter": "string", "target_lufs": "number (default -14)", "container": "mp3|wav|flac (default mp3)", "caption": "string", "name": "string (default mix)"})
def render_and_deliver(core, project_id=None, adapter=None, target_lufs=-14.0, container="mp3", caption="", name="mix", **_):
    _need({"project_id": project_id, "adapter": adapter}, "project_id", "adapter")
    render = core.call("render.start", project_id=project_id, name=name, scope="master", format={"container": container}, target_lufs=float(target_lufs), check=True, wait_s=60)
    if render.get("state") != "succeeded":
        raise RuntimeError("the render did not finish: %s" % render.get("state"))
    result = render["result"]
    sent = core.call("delivery.send", project_id=project_id, source={"job_id": render["job_id"]}, adapter=adapter, caption=caption, wait_s=60)
    return {"render": {"file": result["file"]["path"], "loudness": result.get("loudness"), "stats": result.get("stats")}, "delivery": sent}


@skill("mastering_check", "Renders a preview of the master and reports loudness against a target without changing the project.", {"project_id": "string", "target_lufs": "number (default -14)"})
def mastering_check(core, project_id=None, target_lufs=-14.0, **_):
    _need({"project_id": project_id}, "project_id")
    clip = core.call("render.preview", project_id=project_id, analysis=["loudness"], wait_s=60)
    loud = clip["result"]["loudness"]
    delta = float(target_lufs) - loud["integrated_lufs"]
    return {"integrated_lufs": loud["integrated_lufs"], "true_peak_dbtp": loud["true_peak_dbtp"], "target_lufs": float(target_lufs), "gain_needed_db": round(delta, 2),
            "verdict": "within 1 LU of the target" if abs(delta) <= 1.0 else ("too quiet" if delta > 0 else "too loud")}


def run_skill(core, name, params):
    if name not in REGISTRY:
        raise KeyError("no skill %s (skills: %s)" % (name, ", ".join(sorted(REGISTRY))))
    return REGISTRY[name]["fn"](core, **params)


def describe():
    return [{"name": s["name"], "description": s["description"], "params": s["params"]} for s in REGISTRY.values()]
