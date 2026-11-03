#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Renders a REAPER project (.RPP) with Rovox over MCP-over-HTTP (POST /mcp, Bearer token, Mcp-Session-Id). The project goes through
# `import.rpp` with the FX sidecar (<project>.rovoxfx.json, in the format of tools/reaper/rovox_export_fx.lua) so the real import mapping
# (src/core/ReaperFx.cpp) is what gets tested. The RPP and the sidecar are the ones REAPER itself wrote (reaper_driver.py), the
# audio files are given to the server by path: the harness work directory is mounted into the container (HOST_DIR -> CONT_DIR).
# The rendered file is copied out of the container with `docker cp` (the render lands in the project's renders/ directory).
import json
import os
import re
import subprocess
import urllib.request

URL = os.environ.get("ROVOX_PARITY_URL", "http://127.0.0.1:14820/mcp")
TOKEN = os.environ.get("ROVOX_PARITY_TOKEN", "m3demo")
CONTAINER = os.environ.get("ROVOX_PARITY_CONTAINER", "rovox-parity")
# host directory -> path of the same directory inside the container
HOST_DIR = os.environ.get("ROVOX_PARITY_HOST_DIR", os.path.expanduser("~/.rovox-parity"))
CONT_DIR = os.environ.get("ROVOX_PARITY_CONT_DIR", "/work")


def to_container(path):
    path = os.path.abspath(path)
    return CONT_DIR + path[len(HOST_DIR):] if path.startswith(HOST_DIR + os.sep) else path


class Mcp:
    def __init__(self, url=URL, token=TOKEN):
        self.url, self.token, self.sid, self.n = url, token, None, 0
        self._post({"jsonrpc": "2.0", "id": 0, "method": "initialize",
                    "params": {"protocolVersion": "2025-03-26", "capabilities": {}, "clientInfo": {"name": "rovox-parity", "version": "1"}}})
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _post(self, body):
        h = {"Authorization": "Bearer " + self.token, "Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        if self.sid:
            h["Mcp-Session-Id"] = self.sid
        with urllib.request.urlopen(urllib.request.Request(self.url, json.dumps(body).encode(), h), timeout=900) as r:
            sid = r.headers.get("Mcp-Session-Id")
            if sid and not self.sid:
                self.sid = sid
            text = r.read().decode()
        if not text.strip():
            return None
        if text.lstrip().startswith("event:") or text.lstrip().startswith("data:"):
            text = "".join(l[5:] for l in text.splitlines() if l.startswith("data:"))
        return json.loads(text)

    def call(self, tool, **args):
        """Returns the tool's structured result {"ok", "data"|"error", ...}; raises on JSON-RPC errors."""
        self.n += 1
        r = self._post({"jsonrpc": "2.0", "id": self.n, "method": "tools/call", "params": {"name": tool, "arguments": args}})
        if "error" in r:
            raise RuntimeError("%s: %s" % (tool, r["error"]))
        res = r["result"]
        out = res.get("structuredContent")
        if out is None:
            out = json.loads(res["content"][0]["text"])
        return out

    def ok(self, tool, **args):
        r = self.call(tool, **args)
        if not r.get("ok"):
            raise RuntimeError("%s failed: %s" % (tool, json.dumps(r.get("error", r))[:600]))
        return r.get("data", {})


def prepare_rpp(rpp, work, name):
    """Copy of the RPP for the container: absolute media paths below the harness work dir are rewritten to the mount point."""
    text = open(rpp, encoding="utf-8", errors="surrogateescape").read()
    text = text.replace(HOST_DIR + "/", CONT_DIR + "/")
    out = os.path.join(work, name + ".rovox.RPP")
    with open(out, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(text)
    return out


def render(rpp, sidecar, work, name, tail_s=0.0, start=None, end=None, mcp=None):
    """Imports `rpp` (+ sidecar JSON from REAPER, or None) and renders the master to <work>/<name>.rovox.wav (stereo 48 kHz 32 bit float).
    Returns {"wav": path, "import": report, "render": result}."""
    mcp = mcp or Mcp()
    rpp_c = prepare_rpp(rpp, work, name)
    args = {"path": to_container(rpp_c), "copy_media": False}
    if sidecar:
        side = os.path.join(work, name + ".rovox.rovoxfx.json")
        json.dump(sidecar, open(side, "w"))
        args["fx_sidecar"] = to_container(side)
    pid = mcp.ok("project.new", name="parity_" + name, sample_rate=48000, ephemeral=True)["project_id"]
    try:
        imp = mcp.ok("import.rpp", project_id=pid, **args)
        rargs = {"project_id": pid, "name": name, "scope": "master", "tail_s": tail_s, "wait_s": 60,
                 "format": {"container": "wav", "bit_depth": 32, "channels": 2, "sample_rate": 48000}}
        if tail_s > 0 and start is None:
            # Rovox cuts a render tail at the first silent block (echoes with gaps would be lost); REAPER renders the whole tail, so the
            # window is made explicit: project length + tail, no engine tail
            length = mcp.ok("project.get", project_id=pid, include=["items"])["length_s"]
            start, end, rargs["tail_s"] = 0.0, length + tail_s, 0.0
        if start is not None:
            rargs["range"] = {"start": {"s": start}, "end": {"s": end}}
        res = mcp.ok("render.start", **rargs)
        job = res.get("job_id")
        while job and res.get("state", "succeeded") not in ("succeeded", "failed", "cancelled") and "result" not in res:
            res = mcp.ok("job.get", project_id=pid, job_id=job, wait_s=30)
        if res.get("state") == "failed":
            raise RuntimeError("Rovox render failed: %s" % json.dumps(res.get("error", res))[:600])
        result = res.get("result", res)
        remote = result.get("file", {}).get("abs_path") or _find_path(result)
        out = os.path.join(work, name + ".rovox.wav")
        subprocess.run(["docker", "cp", "%s:%s" % (CONTAINER, remote), out], check=True, capture_output=True)
        return {"wav": out, "import": imp, "render": result}
    finally:
        try:
            mcp.call("project.delete", project_id=pid)
        except Exception:
            pass


def _find_path(obj):
    """The rendered file path in a render result (key names are not fixed: first string that ends in .wav)."""
    if isinstance(obj, str):
        return obj if obj.endswith(".wav") else None
    if isinstance(obj, dict):
        for k in ("path", "file", "output", "uri"):
            if isinstance(obj.get(k), str) and obj[k].endswith(".wav"):
                return re.sub(r"^file://", "", obj[k])
        for v in obj.values():
            r = _find_path(v)
            if r:
                return r
    if isinstance(obj, list):
        for v in obj:
            r = _find_path(v)
            if r:
                return r
    return None
