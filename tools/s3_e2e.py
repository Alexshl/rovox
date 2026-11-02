#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Live check of object storage (S3) as the source of truth, over MCP HTTP with a plain Python client (stdlib only).
#   seed:   create a project (import an .RPP or WAV files, add an effect), save, wait until the bucket has the revision, write a state file.
#   verify: on a NEW server with an EMPTY data volume: project.catalog lists the project from the bucket, project.open is refused while the dead
#           server's writer lease lives and works after it expired, tracks / effects / media are all there, a render lands in the bucket.
# Paths are paths on the SERVER side.   tools/s3_e2e.py seed --http http://127.0.0.1:14811/mcp --token T --rpp "/in/REAPER Media/mykola.RPP" --state /tmp/s3state.json
import argparse
import json
import sys
import time
import urllib.request


class Http:
    def __init__(self, url, token):
        self.url, self.token, self.session, self.next_id = url, token, None, 1

    def _post(self, msg):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        return urllib.request.urlopen(urllib.request.Request(self.url, json.dumps(msg).encode(), headers), timeout=300)

    def rpc(self, method, params=None, notify=False):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notify:
            msg["id"] = self.next_id
        with self._post(msg) as r:
            if r.headers.get("Mcp-Session-Id"):
                self.session = r.headers["Mcp-Session-Id"]
            body = r.read().decode()
            if notify:
                return None
            if "text/event-stream" in r.headers.get("Content-Type", ""):
                for block in body.split("\n\n"):
                    for line in block.splitlines():
                        if line.startswith("data:"):
                            m = json.loads(line[5:])
                            if m.get("id") == msg["id"]:
                                return self._result(m)
                raise RuntimeError("no answer")
            return self._result(json.loads(body))

    @staticmethod
    def _result(m):
        if "error" in m:
            raise RuntimeError(m["error"])
        return m["result"]

    def tool(self, name, /, **args):
        return self.rpc("tools/call", {"name": name, "arguments": args})["structuredContent"]

    def ok(self, name, /, **args):
        r = self.tool(name, **args)
        if not r.get("ok"):
            raise RuntimeError(f"{name} failed: {json.dumps(r.get('error'))}")
        return r["data"]

    def connect(self):
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "s3_e2e.py", "version": "1"}})
        self.rpc("notifications/initialized", notify=True)


def step(text):
    print(f"\n== {text}", flush=True)


def summary(c, pid):
    g = c.ok("project.get", project_id=pid, include=["tracks", "items", "fx"])
    fx = sum(len(v) for v in g.get("fx", {}).values())
    return {"name": g["name"], "revision": g["revision"], "tracks": len(g.get("tracks", [])), "items": len(g.get("items", [])), "fx": fx, "storage": g.get("storage")}


def wait_synced(c, pid, timeout):
    t0 = time.time()
    shown = None
    while time.time() - t0 < timeout:
        g = c.ok("project.get", project_id=pid, include=["tracks"])
        st = g.get("storage") or {}
        if st.get("backend") == "s3" and st.get("pending_uploads") == 0 and st.get("synced_revision") == g["revision"] and not g.get("dirty"):
            return st
        if st.get("last_error") and st["last_error"] != shown:
            shown = st["last_error"]
            print("  sync error:", shown, flush=True)
        time.sleep(1)
    raise RuntimeError("the bucket did not catch up in time: " + json.dumps(st))


def seed(a):
    c = Http(a.http, a.token)
    c.connect()
    step("create the project")
    pid = c.ok("project.new", name=a.name)["project_id"]
    if a.rpp:
        step("import.rpp " + a.rpp)
        r = c.ok("import.rpp", project_id=pid, path=a.rpp)
        print("  imported", r.get("imported"), "| unsupported", len(r.get("unsupported", [])), "| missing media", len(r.get("missing_media", [])))
    else:
        step("import WAV files " + " ".join(a.wav))
        plan = c.ok("import.plan", project_id=pid, paths=a.wav, wait_s=60)
        c.ok("import.apply", project_id=pid, plan_id=plan["result"]["plan_id"])
    tracks = c.ok("track.list", project_id=pid)["tracks"]
    if a.fx:
        step("fx.add compressor on the first track")
        c.ok("fx.add", project_id=pid, track_id=tracks[0]["track_id"], builtin="compressor")
    step("project.save")
    c.ok("project.save", project_id=pid)
    st = wait_synced(c, pid, a.timeout)
    print("  bucket is at revision", st["synced_revision"])
    state = {"project_id": pid, **summary(c, pid)}
    print(json.dumps(state, indent=2))
    json.dump(state, open(a.state, "w"))


def verify(a):
    want = json.load(open(a.state))
    pid = want["project_id"]
    c = Http(a.http, a.token)
    c.connect()
    step("project.catalog on the new server (empty data volume)")
    cat = c.ok("project.catalog")["projects"]
    mine = [p for p in cat if p["project_id"] == pid]
    assert mine, f"the project is not listed from the bucket: {cat}"
    print("  listed:", json.dumps({k: mine[0].get(k) for k in ("name", "tracks", "revision", "remote_only", "open")}))
    assert mine[0].get("remote_only") is True and mine[0]["tracks"] == want["tracks"], mine[0]

    step("project.open (waits out the lease of the killed server)")
    refused = None
    t0 = time.time()
    while True:
        r = c.tool("project.open", target=pid)
        if r.get("ok"):
            break
        err = r["error"]
        if err.get("code") != "BUSY" or "lease" not in err.get("message", ""):
            raise RuntimeError("open failed: " + json.dumps(err))
        refused = refused or err["message"]
        if time.time() - t0 > a.wait_lease:
            raise RuntimeError("the lease never expired: " + json.dumps(err))
        time.sleep(2)
    print(f"  opened after {time.time() - t0:.0f} s; first refusal: {refused}")

    step("everything is back")
    got = summary(c, pid)
    print(json.dumps(got, indent=2))
    for k in ("name", "tracks", "items", "fx"):
        assert got[k] == want[k], f"{k}: {got[k]} != {want[k]}"
    assert got["revision"] == want["revision"], (got["revision"], want["revision"])
    assert got["storage"]["backend"] == "s3"

    step("render lands in the bucket")
    started = c.ok("render.start", project_id=pid, name="s3check", tail_s=0)
    job = started["job_id"]
    while True:
        j = c.ok("job.get", job_id=job, wait_s=5)
        if j["state"] not in ("queued", "running"):
            break
    assert j["state"] == "succeeded", j
    f = j["result"]["file"]
    print("  render:", f["path"], f["bytes"], "bytes; object key:", f.get("key"), "; storage_error:", j["result"].get("storage_error"))
    assert f.get("key") and not j["result"].get("storage_error"), j["result"]
    print(json.dumps({"render_key": f["key"], "refused_first": bool(refused)}))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["seed", "verify"])
    ap.add_argument("--http", required=True)
    ap.add_argument("--token", default="")
    ap.add_argument("--state", required=True)
    ap.add_argument("--name", default="s3-e2e")
    ap.add_argument("--rpp", help="REAPER project on the server side")
    ap.add_argument("--wav", nargs="*", default=[], help="audio files on the server side")
    ap.add_argument("--fx", action="store_true", help="add a built-in compressor")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--wait-lease", type=int, default=90)
    args = ap.parse_args()
    try:
        (seed if args.mode == "seed" else verify)(args)
    except Exception as e:  # noqa: BLE001
        print("S3 E2E FAILED:", e, file=sys.stderr)
        sys.exit(1)
    print("\nS3 E2E OK", args.mode)
