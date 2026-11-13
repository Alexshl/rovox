#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# A fake `ft_jobs` client for the accompaniment.v1 recipe (stdlib only), run against a live server:
#   tools/recipe_pilot_e2e.py --http http://127.0.0.1:4810 --token T [--cancel]
# It does what the Flute Teacher job adapter does: starts recipe.run accompaniment.v1 as an async job with the parts as notes, polls job.get
# (progress "step i/N"), then checks the manifest (intro_offset_ms, beats, files) and downloads every file through its signed url and compares the
# sha256 with the manifest. With --cancel it cancels the job right after the start and checks that the ephemeral project is gone.
import argparse
import hashlib
import json
import sys
import time
import urllib.error
import urllib.request

BPM = 90
NUMERATOR, DENOMINATOR = 3, 4
BARS = 8


def waltz_parts():
    """8 bars of 3/4: melody, chords, bass and drums as notes {pitch, start_beats, length_beats, velocity}."""
    melody, chords, bass, drums = [], [], [], []
    for bar in range(BARS):
        at = bar * NUMERATOR
        melody.append({"pitch": 72 + bar % 3, "start_beats": at + 1, "length_beats": 2, "velocity": 90})
        for pitch in (57, 60, 64):
            chords.append({"pitch": pitch, "start_beats": at, "length_beats": 3, "velocity": 70})
        bass.append({"pitch": 45, "start_beats": at, "length_beats": 1, "velocity": 90})
        for beat in range(NUMERATOR):
            drums.append({"pitch": 36 if beat == 0 else 42, "start_beats": at + beat, "length_beats": 0.25, "velocity": 100 if beat == 0 else 70})
    return {"melody": {"notes": melody}, "chords": {"notes": chords}, "bass": {"notes": bass}, "drums": {"notes": drums}}


class Client:
    def __init__(self, base, token):
        self.base = base.rstrip("/")
        self.token = token
        self.session = None
        self.next_id = 0

    def _headers(self):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        return headers

    def post(self, msg):
        request = urllib.request.Request(self.base + "/mcp", json.dumps(msg).encode(), self._headers())
        with urllib.request.urlopen(request, timeout=180) as r:
            self.session = r.headers.get("Mcp-Session-Id") or self.session
            body = r.read().decode()
            if "id" not in msg:
                return None
            if "text/event-stream" in r.headers.get("Content-Type", ""):
                for block in body.split("\n\n"):
                    for line in block.splitlines():
                        if line.startswith("data:"):
                            m = json.loads(line[5:])
                            if m.get("id") == msg["id"]:
                                return m
                raise RuntimeError("no response in the event stream")
            return json.loads(body)

    def rpc(self, method, params=None):
        self.next_id += 1
        m = self.post({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}})
        if "error" in m:
            raise RuntimeError(f"{method}: {m['error']}")
        return m["result"]

    def tool(self, tool, /, **args):        # positional-only: tools take a `name` argument of their own
        return self.rpc("tools/call", {"name": tool, "arguments": args})["structuredContent"]

    def ok(self, tool, /, **args):
        r = self.tool(tool, **args)
        if not r.get("ok"):
            raise RuntimeError(f"{tool} failed: {json.dumps(r.get('error'))}")
        return r["data"]

    def start(self):
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "recipe_pilot_e2e.py", "version": "1"}})
        self.post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def download(self, url):
        full = url if url.startswith("http") else self.base + url
        with urllib.request.urlopen(urllib.request.Request(full), timeout=180) as r:
            return r.read()


def wait_job(client, job_id, timeout_s):
    deadline = time.time() + timeout_s
    last = None
    while True:
        job = client.ok("job.get", job_id=job_id, wait_s=5)
        if job.get("stage") != last:
            last = job.get("stage")
            print(f"  {job['state']} {job.get('progress', 0):.0%} {last or ''}")
        if job["state"] not in ("queued", "running"):
            return job
        if time.time() > deadline:
            raise RuntimeError("the job did not finish in time")


def has_project(client, name):
    projects = client.ok("project.list", include_ephemeral=True)["projects"]
    return any(p.get("name") == name for p in projects)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", default="http://127.0.0.1:4810", help="server base URL (without /mcp)")
    ap.add_argument("--token", default="")
    ap.add_argument("--cancel", action="store_true", help="cancel right after the start instead of waiting for the result")
    ap.add_argument("--timeout-s", type=float, default=600.0)
    a = ap.parse_args()

    client = Client(a.http, a.token)
    client.start()
    name = "pilot" + str(int(time.time()))
    params = {"name": name, "key": "A minor", "bpm": BPM, "time_signature": {"numerator": NUMERATOR, "denominator": DENOMINATOR},
              "count_in_bars": 1, "click": "separate", "tempo_variants_pct": [100, 80], "parts": waltz_parts(), "seed": "pilot",
              "outputs": ["minus_mp3", "plus_mp3", "click_wav", "midi"]}

    # what ft_jobs sends: the recipe, async, and a client_request_id so that a retry returns the same job
    started = client.ok("recipe.run", recipe_id="accompaniment.v1", params=params, **{"async": True}, client_request_id=name)
    job_id = started["job_id"]
    print("job", job_id)

    if a.cancel:
        client.ok("job.cancel", job_id=job_id)
        job = wait_job(client, job_id, a.timeout_s)
        assert job["state"] == "cancelled", job
        assert job["error"]["code"] == "CANCELLED", job
        assert not has_project(client, "accompaniment " + name), "the ephemeral project is still there"
        print("cancel ok")
        return

    job = wait_job(client, job_id, a.timeout_s)
    assert job["state"] == "succeeded", job
    manifest = job["result"]["result"]

    one_bar_ms = NUMERATOR * 60000.0 / BPM
    assert abs(manifest["intro_offset_ms"] - one_bar_ms) <= 1.0, manifest["intro_offset_ms"]
    assert len(manifest["variants"]) == 2
    for variant in manifest["variants"]:
        expected = NUMERATOR * 60000.0 / variant["bpm"]
        assert abs(variant["intro_offset_ms"] - expected) <= 1.0, variant["intro_offset_ms"]
        assert len(variant["beats"]) == (BARS + 1) * NUMERATOR
        assert abs(variant["beats"][NUMERATOR]["t_ms"] - variant["intro_offset_ms"]) <= 1.0
        for kind in ("minus_mp3", "plus_mp3", "click_wav"):
            f = variant["files"][kind]
            data = client.download(f["url"])
            assert hashlib.sha256(data).hexdigest() == f["sha256"], f"{kind}: sha256 differs from the manifest"
            print(f"  {variant['tempo_pct']}% {kind}: {len(data)} bytes ok")
    assert manifest["midi"], "no midi in the manifest"
    assert manifest["instrument_packs"] and manifest["attributions"]
    print("template", manifest["template"], "packs", [p["id"] for p in manifest["instrument_packs"]])

    client.ok("project.delete", project_id=manifest["ephemeral_project_id"])
    print("pilot ok")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, urllib.error.URLError) as e:
        sys.exit(f"FAILED: {e}")
