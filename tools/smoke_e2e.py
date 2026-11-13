#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Smoke test of a plain running server (stdlib only; no REAPER, no host files, no audio device):
#   tools/smoke_e2e.py --http http://127.0.0.1:4810 --token T
# initialize MCP over HTTP, tools/list, project.new, track.create, a 1-second render.preview (waited), assert a clip is produced, project.delete.
import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", default="http://127.0.0.1:4810", help="server base URL (without /mcp)")
    ap.add_argument("--token", default="")
    ap.add_argument("--wait-health-s", type=float, default=60.0)
    a = ap.parse_args()
    base = a.http.rstrip("/")
    session = None
    next_id = 0

    deadline = time.time() + a.wait_health_s
    while True:
        try:
            with urllib.request.urlopen(base + "/healthz", timeout=3) as r:
                if r.status == 200:
                    print("healthz", r.read().decode())
                    break
        except (urllib.error.URLError, OSError):
            pass
        if time.time() > deadline:
            sys.exit("healthz did not answer in time")
        time.sleep(1)

    def post(msg):
        nonlocal session
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18"}
        if a.token:
            headers["Authorization"] = "Bearer " + a.token
        if session:
            headers["Mcp-Session-Id"] = session
        with urllib.request.urlopen(urllib.request.Request(base + "/mcp", json.dumps(msg).encode(), headers), timeout=120) as r:
            session = r.headers.get("Mcp-Session-Id") or session
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

    def rpc(method, params=None):
        nonlocal next_id
        next_id += 1
        m = post({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params or {}})
        if "error" in m:
            raise RuntimeError(f"{method}: {m['error']}")
        return m["result"]

    def ok(tool, /, **args):        # positional-only: tools take a `name` argument of their own
        r = rpc("tools/call", {"name": tool, "arguments": args})["structuredContent"]
        if not r.get("ok"):
            raise RuntimeError(f"{tool} failed: {json.dumps(r.get('error'))}")
        return r

    init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke_e2e.py", "version": "1"}})
    post({"jsonrpc": "2.0", "method": "notifications/initialized"})
    print("server", init["serverInfo"])
    names = {t["name"] for t in rpc("tools/list")["tools"]}
    for needed in ("project.new", "track.create", "render.preview", "project.delete"):
        assert needed in names, f"tool {needed} missing"
    print(len(names), "tools")

    pid = ok("project.new", name="smoke")["data"]["project_id"]
    try:
        ok("track.create", project_id=pid, name="smoke track")
        job = ok("render.preview", project_id=pid, range={"start": {"s": 0}, "end": {"s": 1}}, format="wav", analysis=["loudness"], wait_s=60)["data"]
        assert job["state"] == "succeeded", job
        clip = job["result"]["clip"]
        assert clip.get("abs_path") or clip.get("path"), clip
        print("preview clip", json.dumps(clip))
    finally:
        ok("project.delete", project_id=pid)
    print("smoke ok")


if __name__ == "__main__":
    main()
