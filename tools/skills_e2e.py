#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Core + rovox-skills sidecar end to end with docker/compose.yml (stdlib only):
#   ROVOX_TOKEN=T tools/skills_e2e.py --compose docker/compose.yml --token T
# Starts the core (`up -d --wait rovox`), uploads a generated 120 BPM click WAV over the tus API, imports it into a project whose tempo is set to
# 100 first, calls the sidecar's `analysis.tempo_map {apply:true}` over its stdio MCP (`docker compose run --rm -T skills`) and checks that
# `tempo.get` on the core now shows 120 +- 0.5.
import argparse
import io
import json
import os
import subprocess
import sys
import time
import urllib.request
import wave

RATE = 44100


def click_wav(seconds, bpm):
    n = int(seconds * RATE)
    period = 60.0 / bpm * RATE
    burst = int(0.004 * RATE)
    frames = bytearray(n * 2)
    seed = 12345
    b = 0
    while b * period < n:
        at = int(b * period)
        for k in range(burst):
            if at + k >= n:
                break
            seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
            v = int(26000 * (1 - k / burst) * ((seed / 0xFFFFFFFF) * 2 - 1))     # a decaying noise burst: a clear onset
            frames[(at + k) * 2:(at + k) * 2 + 2] = v.to_bytes(2, "little", signed=True)
        b += 1
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    return out.getvalue()


class Core:
    def __init__(self, base, token):
        self.base, self.token, self.session, self.n = base.rstrip("/"), token, None, 0

    def _headers(self, extra=None):
        h = dict(extra or {})
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        return h

    def _rpc(self, msg):
        h = self._headers({"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18"})
        if self.session:
            h["Mcp-Session-Id"] = self.session
        with urllib.request.urlopen(urllib.request.Request(self.base + "/mcp", json.dumps(msg).encode(), h), timeout=180) as r:
            self.session = r.headers.get("Mcp-Session-Id") or self.session
            body = r.read().decode()
        if "id" not in msg:
            return None
        lines = [body] if body.lstrip().startswith("{") else [l[5:] for l in body.splitlines() if l.startswith("data:")]
        for line in lines:
            m = json.loads(line)
            if m.get("id") == msg["id"]:
                return m
        raise RuntimeError("no response")

    def init(self):
        self.n += 1
        self._rpc({"jsonrpc": "2.0", "id": self.n, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "skills_e2e.py", "version": "1"}}})
        self._rpc({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def ok(self, name, **args):
        self.n += 1
        m = self._rpc({"jsonrpc": "2.0", "id": self.n, "method": "tools/call", "params": {"name": name, "arguments": args}})
        if "error" in m:
            raise RuntimeError("%s: %s" % (name, m["error"]))
        r = m["result"]["structuredContent"]
        if not r.get("ok"):
            raise RuntimeError("%s failed: %s" % (name, json.dumps(r.get("error"))))
        return r["data"]

    def upload(self, relpath, data):
        h = self._headers({"Content-Type": "application/json"})
        with urllib.request.urlopen(urllib.request.Request(self.base + "/api/upload-batches", json.dumps({"files": [{"relpath": relpath, "size": len(data)}]}).encode(), h), timeout=60) as r:
            batch = json.loads(r.read())
        url = batch["files"][0]["url"]
        h = self._headers({"Tus-Resumable": "1.0.0", "Content-Type": "application/offset+octet-stream", "Upload-Offset": "0"})
        urllib.request.urlopen(urllib.request.Request(self.base + url, data, h, method="PATCH"), timeout=120).close()
        return self.ok("upload.finish", batch_id=batch["batch_id"])["path"]


def sidecar_call(compose, env, project_id, timeout):
    """One tools/call against the sidecar over its stdio MCP, the way an agent starts it."""
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "skills_e2e.py", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "analysis.tempo_map", "arguments": {"project_id": project_id, "source": "master", "apply": True}}},
    ]
    p = subprocess.run(["docker", "compose", "-f", compose, "run", "--rm", "-T", "skills"], input="\n".join(json.dumps(m) for m in msgs) + "\n", capture_output=True, text=True, env=env, timeout=timeout)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise RuntimeError("the sidecar exited with %d" % p.returncode)
    for line in p.stdout.splitlines():
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if m.get("id") == 2:
            return m["result"]["structuredContent"]
    raise RuntimeError("no answer from the sidecar: " + p.stdout[-500:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compose", default="docker/compose.yml")
    ap.add_argument("--token", default=os.environ.get("ROVOX_TOKEN", ""))
    ap.add_argument("--url", default="http://127.0.0.1:4810", help="where the core is published")
    ap.add_argument("--bpm", type=float, default=120.0)
    ap.add_argument("--no-up", action="store_true", help="the core is already running")
    a = ap.parse_args()
    if not a.token:
        sys.exit("set ROVOX_TOKEN or pass --token")
    env = dict(os.environ, ROVOX_TOKEN=a.token)

    if not a.no_up:
        subprocess.run(["docker", "compose", "-f", a.compose, "up", "-d", "--wait", "--no-build", "rovox"], check=True, env=env)
    core = Core(a.url, a.token)
    deadline = time.time() + 60
    while True:
        try:
            urllib.request.urlopen(a.url + "/healthz", timeout=3).close()
            break
        except OSError:
            if time.time() > deadline:
                sys.exit("the core did not answer on /healthz")
            time.sleep(1)
    core.init()

    pid = core.ok("project.new", name="skills-e2e")["project_id"]
    try:
        folder = core.upload("Click.wav", click_wav(30, a.bpm))
        plan = core.ok("import.plan", project_id=pid, paths=[folder], preset="auto", takes_mode="separate")
        core.ok("import.apply", project_id=pid, plan_id=plan["plan_id"])
        core.ok("tempo.set", project_id=pid, points=[{"bpm": 100}])     # so that 120 can only come from the sidecar
        before = core.ok("tempo.get", project_id=pid)["points"][0]["bpm"]
        assert abs(before - 100) < 0.01, before

        r = sidecar_call(a.compose, env, pid, timeout=300)
        if not r.get("ok"):
            sys.exit("analysis.tempo_map failed: " + json.dumps(r.get("error")))
        print("sidecar:", json.dumps(r["data"]))
        assert r["data"].get("applied") is True, r

        after = core.ok("tempo.get", project_id=pid)["points"][0]["bpm"]
        print("tempo.get on the core: %.3f BPM" % after)
        if abs(after - a.bpm) > 0.5:
            sys.exit("tempo.get shows %.3f BPM, expected %.1f +- 0.5" % (after, a.bpm))
    finally:
        try:
            core.ok("project.delete", project_id=pid)
        except Exception as e:      # the check above is what matters; do not hide its failure behind a cleanup error
            print("cleanup failed:", e, file=sys.stderr)
    print("skills e2e ok")


if __name__ == "__main__":
    main()
