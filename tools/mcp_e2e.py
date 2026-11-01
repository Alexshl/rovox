#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Live end-to-end check of the MCP server with a plain Python client (stdlib only).
#   stdio:  tools/mcp_e2e.py --zprj /in/song.zprj --stdio docker run -i --rm -v data:/data -v ~/songs:/in:ro rovox mcp
#   http:   tools/mcp_e2e.py --zprj /in/song.zprj --http http://127.0.0.1:4810/mcp [--token T]
# The .zprj path is a path on the SERVER side. Steps: import, list tracks, set volume/pan, add a built-in FX,
# render with a LUFS target as a job (polled to done), undo.
import argparse
import json
import subprocess
import sys
import time
import urllib.request


class Stdio:
    def __init__(self, cmd):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.notifications = []

    def request(self, msg):
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("server closed stdout")
            m = json.loads(line)  # every stdout line must be JSON
            if "id" in m and m.get("id") == msg.get("id") and ("result" in m or "error" in m):
                return m
            self.notifications.append(m)

    def notify(self, msg):
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=30)


class Http:
    def __init__(self, url, token):
        self.url, self.token, self.session = url, token, None
        self.notifications = []

    def _post(self, msg):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        req = urllib.request.Request(self.url, json.dumps(msg).encode(), headers)
        return urllib.request.urlopen(req, timeout=120)

    def request(self, msg):
        with self._post(msg) as r:
            if r.headers.get("Mcp-Session-Id"):
                self.session = r.headers["Mcp-Session-Id"]
            body = r.read().decode()
            if "text/event-stream" in r.headers.get("Content-Type", ""):
                result = None
                for block in body.split("\n\n"):
                    for line in block.splitlines():
                        if line.startswith("data:"):
                            m = json.loads(line[5:])
                            if m.get("id") == msg["id"]:
                                result = m
                            else:
                                self.notifications.append(m)
                return result
            return json.loads(body)

    def notify(self, msg):
        with self._post(msg):
            pass

    def close(self):
        if self.session:
            headers = {"Mcp-Session-Id": self.session}
            if self.token:
                headers["Authorization"] = "Bearer " + self.token
            urllib.request.urlopen(urllib.request.Request(self.url, headers=headers, method="DELETE"), timeout=10).close()


class Client:
    def __init__(self, transport):
        self.t, self.next_id = transport, 1

    def rpc(self, method, params=None):
        self.next_id += 1
        m = self.t.request({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}})
        if "error" in m:
            raise RuntimeError(f"{method}: {m['error']}")
        return m["result"]

    def tool(self, name, /, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        return r["structuredContent"]

    def ok(self, name, /, **args):
        r = self.tool(name, **args)
        if not r.get("ok"):
            raise RuntimeError(f"{name} failed: {json.dumps(r.get('error'))}")
        return r


def step(text):
    print(f"\n== {text}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zprj", required=True, help="path of the .zprj folder as the server sees it")
    ap.add_argument("--http", help="MCP endpoint URL")
    ap.add_argument("--token", default="")
    ap.add_argument("--target-lufs", type=float, default=-16.0)
    ap.add_argument("--bench", type=int, default=0, help="also time N light tool calls (project.list) and print p50/p99")
    ap.add_argument("--stdio", nargs=argparse.REMAINDER, help="command that starts `rovox mcp`")
    a = ap.parse_args()

    transport = Http(a.http, a.token) if a.http else Stdio(a.stdio)
    c = Client(transport)

    step("initialize")
    init = c.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "mcp_e2e.py", "version": "1"}})
    transport.notify({"jsonrpc": "2.0", "method": "notifications/initialized"})
    print("server", init["serverInfo"], "protocol", init["protocolVersion"])
    tools = c.rpc("tools/list")["tools"]
    print(len(tools), "tools")

    step("project.new + import.plan (job) + import.apply")
    pid = c.ok("project.new", name="e2e")["data"]["project_id"]
    t0 = time.time()
    plan = c.ok("import.plan", project_id=pid, paths=[a.zprj], wait_s=60)["data"]
    assert plan["state"] == "succeeded", plan
    print(f"plan {plan['result']['plan_id']}: {len(plan['result']['tracks'])} tracks, {len(plan['result']['warnings'])} warnings, {time.time() - t0:.1f} s")
    applied = c.ok("import.apply", project_id=pid, plan_id=plan["result"]["plan_id"])
    print("apply:", len(applied["data"]["tracks"]), "tracks, revision", applied["revision"])

    step("track.list")
    tracks = c.ok("track.list", project_id=pid)["data"]["tracks"]
    for t in tracks:
        print(f"  {t['track_id']}  {t['name']:<14} {t['volume_db']:6.1f} dB  pan {t['pan']:+.2f}  items {len(t['items'])}")
    first, second = tracks[0], tracks[1]

    step("track.update (two tracks, one call = one undo step)")
    steps_before = len(c.ok("history.list", project_id=pid)["data"]["undo"])
    upd = c.ok("track.update", project_id=pid, undo_label="e2e balance", updates=[
        {"track_id": first["track_id"], "volume_db": -4.0, "pan": -0.3},
        {"track_id": second["track_id"], "volume_db": -6.5, "pan": 0.3}])
    steps_after = len(c.ok("history.list", project_id=pid)["data"]["undo"])
    assert steps_after == steps_before + 1, (steps_before, steps_after)
    now = {t["track_id"]: t for t in c.ok("track.list", project_id=pid)["data"]["tracks"]}
    assert abs(now[first["track_id"]]["volume_db"] + 4.0) < 0.05 and abs(now[second["track_id"]]["pan"] - 0.3) < 0.01, now
    print("revision", upd["revision"], "- undo steps", steps_before, "->", steps_after)

    step("fx.add (built-in compressor) + fx.update")
    fx = c.ok("fx.add", project_id=pid, track_id=first["track_id"], builtin="compressor")
    plugin_id = fx["data"]["plugin_id"]
    params = c.ok("fx.params", project_id=pid, plugin_id=plugin_id)["data"]["fx"]["params"]
    print("  compressor params:", ", ".join(p["name"] for p in params[:6]), "...")
    thr = next(p for p in params if "thresh" in p["name"].lower())
    c.ok("fx.update", project_id=pid, plugin_id=plugin_id, params={thr["ident"]: thr["min"] + (thr["max"] - thr["min"]) * 0.4})

    step(f"render.start target_lufs {a.target_lufs} as a job")
    started = c.ok("render.start", project_id=pid, name="e2e", target_lufs=a.target_lufs, tail_s=0)
    job = started["data"]["job_id"]
    print("job", job, started["data"]["state"])
    last = ""
    t0 = time.time()
    while True:
        j = c.ok("job.get", job_id=job, wait_s=5)["data"]
        line = f"{j['state']} {j['progress'] * 100:4.0f}% {j['stage']}"
        if line != last:
            print("  ", line, flush=True)
            last = line
        if j["state"] not in ("queued", "running"):
            break
    assert j["state"] == "succeeded", j
    res = j["result"]
    print(f"  done in {time.time() - t0:.1f} s: {res['file']['path']} {res['file']['bytes'] // 1024} KiB, {res['file']['duration_s']:.1f} s")
    print(f"  integrated {res['stats']['integrated_lufs']} LUFS (target {a.target_lufs}), true peak {res['stats']['true_peak_dbtp']} dBTP, "
          f"loudness before {res['loudness']['before_lufs']} after {res['loudness']['after_lufs']} gain {res['loudness']['gain_db']:.2f} dB, "
          f"on_target {res['loudness']['on_target']}, clipped {res['stats']['clipped_samples']}")
    print("  preflight warnings:", [w["check"] for w in res["preflight"]["warnings"]])

    step("undo")
    before = {t["track_id"]: t["volume_db"] for t in c.ok("track.list", project_id=pid)["data"]["tracks"]}
    u = c.ok("undo", project_id=pid)  # undoes fx.update
    u2 = c.ok("undo", project_id=pid)  # undoes fx.add
    u3 = c.ok("undo", project_id=pid)  # undoes the track.update
    print("  undone:", u["data"]["undone_label"], "|", u2["data"]["undone_label"], "|", u3["data"]["undone_label"])
    after = {t["track_id"]: t for t in c.ok("track.list", project_id=pid)["data"]["tracks"]}
    assert abs(after[first["track_id"]]["volume_db"] - first["volume_db"]) < 0.05, after[first["track_id"]]
    assert abs(after[second["track_id"]]["pan"] - second["pan"]) < 0.01
    print(f"  first track volume back to {after[first['track_id']]['volume_db']:.1f} dB (was {before[first['track_id']]:.1f} before the undo)")

    step("render.preview 4 bars with analysis")
    pv = c.ok("render.preview", project_id=pid, range={"start": {"bar": 1}, "end": {"bar": 5}}, wait_s=60)["data"]
    r = pv["result"]
    print(f"  clip {r['clip']['duration_s']:.2f} s, {r['loudness']['integrated_lufs']} LUFS, bands {r['spectrum_bands_db']}, bars {len(r['per_bar'])}")

    if a.bench:
        step(f"latency of {a.bench} x project.list")
        ms = []
        for _ in range(a.bench):
            t0 = time.perf_counter()
            c.ok("project.list")
            ms.append((time.perf_counter() - t0) * 1000)
        ms.sort()
        print(f"  p50 {ms[len(ms) // 2]:.2f} ms, p99 {ms[int(len(ms) * 0.99)]:.2f} ms, max {ms[-1]:.2f} ms")

    transport.close()
    print("\nE2E OK")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # noqa: BLE001
        print("E2E FAILED:", e, file=sys.stderr)
        sys.exit(1)
