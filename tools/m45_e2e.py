#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Live end-to-end check of milestones M4 and M5 against a running `rovox serve` (run it inside the runtime container, see docs of the report):
#   import a Zoom R20 .zprj, add LV2 instrument tracks (drum kit + bass synth) from the plugin catalog, write a MIDI drum / bass pattern,
#   put a send on a reverb bus, render at -14 LUFS to MP3, deliver it to a local mock webhook (HMAC checked), then run a rovox-skills skill.
# Standard library plus the rovox_skills package only. No real network: the "webhook" is a thread of this script.
import argparse
import hashlib
import hmac
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "skills"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rovox_skills import skills  # noqa: E402
from rovox_skills.core import Core, CoreError  # noqa: E402

SECRET = "e2e-webhook-secret-" + hashlib.sha1(str(time.time()).encode()).hexdigest()[:8]
received = []


class Hook(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        received.append({"headers": dict(self.headers), "body": body, "path": self.path})
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"{}")

    def log_message(self, *a):
        pass


def parse_multipart(headers, body):
    boundary = headers["Content-Type"].split("boundary=")[1].encode()
    parts = {}
    for chunk in body.split(b"--" + boundary):
        if b"\r\n\r\n" not in chunk:
            continue
        head, _, data = chunk.partition(b"\r\n\r\n")
        data = data[:-2] if data.endswith(b"\r\n") else data
        name = head.decode(errors="replace").split('name="')[1].split('"')[0]
        parts[name] = data
    return parts


checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(("  ok    " if ok else "  FAIL  ") + name + (("  " + str(detail)) if detail else ""), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:4810/mcp")
    ap.add_argument("--zprj", required=True, help="path of the .zprj folder as the SERVER sees it")
    ap.add_argument("--config", default="/data/config/rovox.toml", help="rovox.toml the server reads (written here)")
    ap.add_argument("--target-lufs", type=float, default=-14.0)
    ap.add_argument("--hook-port", type=int, default=9099)
    a = ap.parse_args()

    hook = HTTPServer(("127.0.0.1", a.hook_port), Hook)
    threading.Thread(target=hook.serve_forever, daemon=True).start()
    os.makedirs(os.path.dirname(a.config), exist_ok=True)
    # the secret goes into a file named by the config (never inline); the adapter list shows no secret
    secret_file = os.path.join(os.path.dirname(a.config), "webhook.secret")
    with open(secret_file, "w") as f:
        f.write(SECRET + "\n")
    os.chmod(secret_file, 0o600)
    with open(a.config, "w") as f:
        f.write('[delivery.webhook.mock]\nurl = "http://127.0.0.1:%d/hook"\nhmac_secret_file = "%s"\n' % (a.hook_port, secret_file))

    core = Core(a.server, name="skill:m45-e2e")
    print("== catalog")
    for _ in range(60):
        cat = core.call("plugin.catalog", kind="instrument", limit=100)
        if cat["scan"]["state"] == "done":
            break
        time.sleep(1)
    names = [p["name"] for p in cat["plugins"]]
    check("plugin catalog lists instruments", len(names) > 0, "%d instruments, scan %s" % (cat["total"], cat["scan"]["state"]))
    drums = core.call("plugin.catalog", kind="instrument", query="Black Pearl Drumkit", limit=1)["plugins"]
    bass = core.call("plugin.catalog", kind="instrument", query="ZynAddSubFX", limit=1)["plugins"]
    check("drum kit and bass synth found", drums and bass, (drums or [{}])[0].get("name"))
    if not (drums and bass):
        return 1

    print("== import")
    project = core.call("project.new", name="m45 e2e")["project_id"]
    plan = core.call("import.plan", project_id=project, paths=[a.zprj], wait_s=60)
    plan_id = plan["result"]["plan_id"]
    applied = core.call("import.apply", project_id=project, plan_id=plan_id)
    tracks = core.call("track.list", project_id=project)["tracks"]
    check("zprj imported as named tracks", len(tracks) >= 2, ", ".join(t["name"] for t in tracks))
    base_tracks = [t["track_id"] for t in tracks]

    print("== instruments and MIDI")
    drum_track = core.call("track.create", project_id=project, name="E2E Drums")["track_id"]
    added = core.call("fx.add", project_id=project, track_id=drum_track, plugin={"plugin_uid": drums[0]["plugin_uid"]})
    check("drum kit loaded (probed in a child process first)", added["plugin"]["loaded"] and added["plugin"]["is_instrument"], added["plugin"].get("probe"))
    bass_track = core.call("track.create", project_id=project, name="E2E Bass")["track_id"]
    added = core.call("fx.add", project_id=project, track_id=bass_track, plugin={"plugin_uid": bass[0]["plugin_uid"]})
    check("bass synth loaded", added["plugin"]["loaded"] and added["plugin"]["is_instrument"], added["plugin"].get("probe"))
    d = core.call("item.create_midi", project_id=project, track_id=drum_track, start={"bar": 1}, name="drums", notes=skills.drum_pattern(4, seed=11))
    b = core.call("item.create_midi", project_id=project, track_id=bass_track, start={"bar": 1}, name="bass", notes=skills.bass_pattern(36, 4, seed=11))
    check("MIDI written to both tracks", d["item"]["notes"] > 20 and b["item"]["notes"] > 5, "%d drum notes, %d bass notes" % (d["item"]["notes"], b["item"]["notes"]))
    got = core.call("midi.get", project_id=project, item_id=b["item_id"])
    check("midi.get reads the notes back", len(got["notes"]) == b["item"]["notes"])

    print("== send to a reverb bus")
    bus = core.call("track.create", project_id=project, name="E2E Reverb")["track_id"]
    core.call("fx.add", project_id=project, track_id=bus, builtin="reverb")
    sends = [core.call("routing.send_create", project_id=project, track_id=t, to_track_id=bus, gain_db=-12.0)["send_id"] for t in (drum_track, bass_track, base_tracks[0])]
    routing = core.call("routing.list", project_id=project)
    check("sends created", len(routing["sends"]) == 3 and len(routing["returns"]) == 1)

    print("== undo of an agent step")
    core.call("undo", project_id=project)
    check("undo removes the last send", len(core.call("routing.list", project_id=project)["sends"]) == 2)
    core.call("redo", project_id=project)

    print("== render at %.0f LUFS to MP3" % a.target_lufs)
    render = core.call("render.start", project_id=project, name="e2e", scope="master", format={"container": "mp3", "bitrate_kbps": 192}, target_lufs=a.target_lufs, check=True, wait_s=60)
    for _ in range(120):
        if render["state"] in ("succeeded", "failed", "cancelled"):
            break
        render = core.call("job.get", job_id=render["job_id"], wait_s=5)
    check("render job succeeded", render["state"] == "succeeded", render.get("error"))
    if render["state"] != "succeeded":
        return 1
    result = render["result"]
    loud = result["loudness"]
    print("   loudness:", json.dumps(loud))
    check("loudness target reached (+-1 LU)", abs(result["stats"]["integrated_lufs"] - a.target_lufs) <= 1.0, "%.2f LUFS" % result["stats"]["integrated_lufs"])
    check("file is an mp3", result["file"]["path"].endswith(".mp3") and result["file"]["bytes"] > 10000, result["file"]["bytes"])

    print("== the instruments are audible in the mix")
    analysis = core.call("analyze.audio", project_id=project, kinds=["tracks_presence"], wait_s=60)
    for _ in range(60):
        if analysis["state"] in ("succeeded", "failed", "cancelled"):
            break
        analysis = core.call("job.get", job_id=analysis["job_id"], wait_s=5)
    presence = {t["name"]: t for t in analysis.get("result", {}).get("tracks", [])}
    print("   rms by track:", {n: t["rms_dbfs"] for n, t in presence.items()})
    check("drum and bass tracks carry sound", presence.get("E2E Drums", {}).get("rms_dbfs", -200) > -60 and presence.get("E2E Bass", {}).get("rms_dbfs", -200) > -60)

    print("== deliver to the mock webhook")
    adapters = core.call("delivery.list_adapters")["adapters"]
    check("webhook.mock listed without secrets", any(x["id"] == "webhook.mock" for x in adapters) and SECRET not in json.dumps(adapters))
    sent = core.call("delivery.send", project_id=project, source={"job_id": render["job_id"]}, adapter="webhook.mock", caption="m45 e2e mix", title="E2E", wait_s=60)
    check("delivery sent", sent["status"] == "sent", sent)
    check("the mock received one request", len(received) == 1)
    if received:
        r = received[0]
        parts = parse_multipart(r["headers"], r["body"])
        file_hash = hashlib.sha256(parts["file"]).hexdigest()
        ts = r["headers"]["X-Rovox-Timestamp"]
        expected = "sha256=" + hmac.new(SECRET.encode(), (ts + "." + file_hash).encode(), hashlib.sha256).hexdigest()
        check("HMAC signature valid", hmac.compare_digest(expected, r["headers"]["X-Rovox-Signature"]))
        check("the delivered bytes are an mp3", parts["file"][:3] == b"ID3" or parts["file"][:2] == b"\xff\xfb" or parts["file"][:2] == b"\xff\xf3", len(parts["file"]))
        check("caption arrived", parts["caption"] == b"m45 e2e mix")
    journal = core.call("delivery.list", project_id=project)
    check("journal has the delivery, no secret in it", len(journal["deliveries"]) == 1 and SECRET not in json.dumps(journal))

    print("== hold")
    core.call("project.update", project_id=project, delivery_hold=True)
    held = core.call("delivery.send", project_id=project, source={"job_id": render["job_id"]}, adapter="webhook.mock")
    check("hold queues the file", held["status"] == "HELD" and len(received) == 1)
    core.call("project.update", project_id=project, delivery_hold=False)
    rel = core.call("delivery.release", project_id=project, wait_s=60)
    check("release sends it exactly once", len(rel["released"]) == 1 and len(received) == 2)

    print("== rovox-skills: render_and_deliver, then rhythm_section on a second project")
    skill_core = Core(a.server, name="skill:render_and_deliver")
    out = skills.run_skill(skill_core, "render_and_deliver", {"project_id": project, "adapter": "webhook.mock", "target_lufs": a.target_lufs, "container": "mp3", "caption": "from the sidecar", "name": "skill"})
    check("skill render_and_deliver delivered", out["delivery"]["status"] == "sent" and len(received) == 3, out["delivery"]["status"])
    second = core.call("project.new", name="m45 e2e rhythm")["project_id"]
    report = skills.run_skill(Core(a.server, name="skill:rhythm_section"), "rhythm_section", {"project_id": second, "bars": 2, "seed": 3})
    check("skill rhythm_section wrote drums and bass", set(report["items"]) == {"Drums", "Bass"}, report["items"])
    activity = core.call("project.activity", project_id=second, limit=50)["activity"]
    check("skill steps are in the journal with origin skill:rhythm_section", any(e["origin"]["kind"] == "skill" and e["origin"]["label"] == "skill:rhythm_section" for e in activity))
    history = core.call("history.list", project_id=second)
    check("skill steps are undoable", len(history["undo"]) >= 2)
    core.call("undo", project_id=second)

    crash = core.call("plugin.catalog", query="Rovox Test Crash Late", limit=1)["plugins"]
    if crash:
        print("== a plugin that crashes during a render (fixture plugin on /plugins/lv2)")
        victim = core.call("project.new", name="m45 e2e crash")["project_id"]
        vt = core.call("track.create", project_id=victim, name="Victim")["track_id"]
        core.call("fx.add", project_id=victim, track_id=vt, plugin={"plugin_uid": crash[0]["plugin_uid"]})
        core.call("item.create_midi", project_id=victim, track_id=vt, notes=skills.bass_pattern(36, 8, seed=1))
        envelope = core.envelope("render.start", project_id=victim, name="boom", wait_s=60)
        err = envelope.get("error") or {}
        check("the render fails with PLUGIN_CRASHED, not a dead server", envelope.get("ok") is False and err.get("code") == "PLUGIN_CRASHED", err.get("message", "")[:140])
        check("the server is alive and the other project untouched", len(core.call("track.list", project_id=project)["tracks"]) >= 4)
        events = core.call("events.poll", since_event_id=0, limit=500)["events"]
        check("a plugin.crashed event was published", any(e.get("type") == "plugin.crashed" for e in events))

    print("== project catalog")
    catalog = core.call("project.catalog")["projects"]
    check("project.catalog lists the projects", any(p["project_id"] == project for p in catalog))

    failed = [c for c in checks if not c[1]]
    print("\n%d checks, %d failed" % (len(checks), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except CoreError as e:
        print("FAIL: %s %s" % (e, e.details))
        sys.exit(1)
