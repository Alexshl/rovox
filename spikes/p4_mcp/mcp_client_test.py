#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Scripted MCP client for the P4 spike. Spawns the server (stdio) and runs the scenarios from task 070.

usage: mcp_client_test.py --cmd "rovox_spikes mcp --work /tmp/p4" [--calls 10000] [--json out.json]
"""
import argparse
import json
import shlex
import statistics
import subprocess
import threading
import time


class Client:
    def __init__(self, cmd):
        self.p = subprocess.Popen(shlex.split(cmd), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
        self.lock = threading.Condition()
        self.responses = {}
        self.notifications = []
        self.bad_lines = 0
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        for raw in iter(self.p.stdout.readline, b""):
            now = time.perf_counter()
            try:
                m = json.loads(raw)
            except ValueError:
                self.bad_lines += 1
                continue
            with self.lock:
                if "id" in m and ("result" in m or "error" in m):
                    key = json.dumps(m["id"])
                    self.responses.setdefault(key, []).append((m, now))
                else:
                    self.notifications.append(m)
                self.lock.notify_all()

    def send_raw(self, line):
        self.p.stdin.write(line.encode() + b"\n")

    def send(self, method, params=None, id=None, notify=False):
        m = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            m["params"] = params
        if not notify:
            m["id"] = id
        self.send_raw(json.dumps(m))
        return time.perf_counter()

    def wait(self, id, timeout=30):
        key = json.dumps(id)
        end = time.time() + timeout
        with self.lock:
            while key not in self.responses:
                left = end - time.time()
                if left <= 0:
                    raise TimeoutError(f"no response for id {id!r}")
                self.lock.wait(left)
            return self.responses[key][0]

    def call(self, id, method, params=None):
        t0 = self.send(method, params, id)
        m, t1 = self.wait(id)
        return m, (t1 - t0) * 1000.0

    def tool(self, id, name, args=None):
        m, ms = self.call(id, "tools/call", {"name": name, "arguments": args or {}})
        return m["result"]["structuredContent"], m["result"]["isError"], ms


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True)
    ap.add_argument("--calls", type=int, default=10000)
    ap.add_argument("--json")
    a = ap.parse_args()
    r = {}
    c = Client(a.cmd)

    # 1. handshake
    m, ms = c.call("init-1", "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "p4-test", "version": "0"}})
    c.send("notifications/initialized", notify=True)
    r["initialize"] = {"ms": ms, "protocolVersion": m["result"]["protocolVersion"], "serverInfo": m["result"]["serverInfo"]}
    m, _ = c.call(2, "tools/list")
    r["tools"] = [t["name"] for t in m["result"]["tools"]]
    r["ping_ok"] = c.call("p", "ping")[0]["result"] == {}
    err, _ = c.call(3, "no/such")
    r["unknown_method_code"] = err["error"]["code"]
    c.send_raw("{not json")
    time.sleep(0.3)
    with c.lock:
        r["bad_json_error"] = [x for x, _ in c.responses.get("null", [])][:1]

    # 2. setup: two projects, one track with a sine each
    pl, _, _ = c.tool(10, "project.list")
    A, B = pl["projects"][0]["project_id"], pl["projects"][1]["project_id"]
    ta, _, _ = c.tool(11, "track.add", {"project_id": A, "name": "Flute", "sine_hz": 440})
    tb, _, _ = c.tool(12, "track.add", {"project_id": B, "name": "Bass", "sine_hz": 110})
    TA, TB = ta["track_id"], tb["track_id"]
    r["track_add_actions_in_one_transaction"] = ta["actions_in_transaction"]
    r["track_add_undo_description"] = ta["undo_description"]

    # 3. sequential round trips (p50/p99), alternating projects, ids > 1000 (ints and strings)
    lat = []
    t_start = time.perf_counter()
    for i in range(a.calls):
        pid, tid = (A, TA) if i % 2 == 0 else (B, TB)
        rid = 100000 + i if i % 3 else f"req-{i}"
        res, err_flag, ms = c.tool(rid, "track.set_volume_db", {"project_id": pid, "track_id": tid, "volume_db": -6.0 - (i % 10)})
        assert not err_flag, res
        lat.append(ms)
    total = time.perf_counter() - t_start
    r["sequential"] = {"calls": a.calls, "total_s": total, "calls_per_s": a.calls / total,
                       "p50_ms": statistics.median(lat), "p99_ms": pct(lat, 0.99), "max_ms": max(lat)}

    # 4. pipelined: 2000 requests in flight, every response matches its id exactly once
    ids = [f"pipe-{i}" if i % 2 else 200000 + i for i in range(2000)]
    t0 = time.perf_counter()
    for i, rid in enumerate(ids):
        pid, tid = (A, TA) if i % 2 == 0 else (B, TB)
        c.send("tools/call", {"name": "track.set_volume_db", "arguments": {"project_id": pid, "track_id": tid, "volume_db": -20.0 + (i % 5)}}, rid)
    for rid in ids:
        c.wait(rid, 60)
    t1 = time.perf_counter()
    with c.lock:
        dup = sum(1 for rid in ids if len(c.responses[json.dumps(rid)]) != 1)
    r["pipelined"] = {"calls": len(ids), "total_ms": (t1 - t0) * 1000, "duplicates_or_missing": dup}

    # 5. undo isolation: A gets a new track + volume change, undo twice, B untouched
    b_before, _, _ = c.tool(20, "track.list", {"project_id": B})
    a_before, _, _ = c.tool(21, "track.list", {"project_id": A})
    new, _, _ = c.tool(22, "track.add", {"project_id": A, "name": "Violin"})
    vol, _, _ = c.tool(23, "track.set_volume_db", {"project_id": A, "track_id": new["track_id"], "volume_db": -12})
    u1, _, _ = c.tool(24, "undo", {"project_id": A})
    mid, _, _ = c.tool(25, "track.list", {"project_id": A})
    u2, _, _ = c.tool(26, "undo", {"project_id": A})
    a_after, _, _ = c.tool(27, "track.list", {"project_id": A})
    b_after, _, _ = c.tool(28, "track.list", {"project_id": B})
    vols = lambda x: [(t["track_id"], round(t["volume_db"], 3)) for t in x["tracks"]]
    r["undo"] = {
        "undone_1": u1["undone_description"], "undone_2": u2["undone_description"],
        "tracks_after_first_undo": len(mid["tracks"]), "tracks_after_second_undo": len(a_after["tracks"]),
        "A_state_restored": vols(a_after) == vols(a_before), "B_unchanged_by_A_undo": vols(b_after) == vols(b_before),
        "A_revision_before": a_before["revision"], "A_revision_after": a_after["revision"], "B_revision_unchanged": b_before["revision"] == b_after["revision"],
    }

    # 6. if_revision conflict
    cur, _, _ = c.tool(30, "track.list", {"project_id": A})
    res, is_err, _ = c.tool(31, "track.set_volume_db", {"project_id": A, "track_id": TA, "volume_db": -1, "if_revision": cur["revision"] - 1})
    ok, ok_err, _ = c.tool(32, "track.set_volume_db", {"project_id": A, "track_id": TA, "volume_db": -1, "if_revision": cur["revision"]})
    r["revision_conflict"] = {"stale_is_error": is_err, "code": res.get("code"), "fresh_ok": not ok_err}

    # 7. long call blocks others on the message thread
    c.send("tools/call", {"name": "debug.sleep", "arguments": {"ms": 2000}}, "long")
    time.sleep(0.2)
    res, _, ms = c.tool(40, "project.list")
    c.wait("long")
    r["head_of_line_blocking"] = {"sleep_ms": 2000, "project.list_wait_ms_during_sleep": ms}

    # 8. UI-origin burst in A while MCP hammers B and A
    ev0, _, _ = c.tool(50, "events.poll", {"since": 0, "limit": 1})
    base_id = ev0["last_event_id"]
    c.tool(51, "debug.ui_burst", {"project_id": A, "track_id": TA, "count": 300, "interval_us": 300})
    for i in range(300):
        pid, tid = (B, TB) if i % 2 else (A, TA)
        c.tool(1000 + i, "track.set_volume_db", {"project_id": pid, "track_id": tid, "volume_db": -30 + (i % 7)})
    time.sleep(1.5)
    evs, _, _ = c.tool(52, "events.poll", {"since": base_id, "limit": 100000})
    by = {}
    for e in evs["events"]:
        by[(e["project_id"], e["origin"])] = by.get((e["project_id"], e["origin"]), 0) + 1
    r["ui_burst"] = {"ui_events_A": by.get((A, "ui"), 0), "mcp_events_A": by.get((A, "mcp:s1"), 0), "mcp_events_B": by.get((B, "mcp:s1"), 0),
                     "system_events": sum(v for (p, o), v in by.items() if o == "system"), "expected_ui": 300, "expected_mcp_A": 150, "expected_mcp_B": 150}

    # 9. global invariants over all events: one event per mutation, per-project revision strictly +1
    allev, _, _ = c.tool(60, "events.poll", {"since": 0, "limit": 10**7})
    ids_seen = [e["event_id"] for e in allev["events"]]
    rev_ok, last = True, {}
    for e in allev["events"]:
        if e["project_id"] in last and e["revision"] != last[e["project_id"]] + 1:
            rev_ok = False
        last[e["project_id"]] = e["revision"]
    origins = {}
    for e in allev["events"]:
        origins[e["origin"]] = origins.get(e["origin"], 0) + 1
    sys_tools = sorted({e["tool"] for e in allev["events"] if e["origin"] == "system"})
    r["events_total"] = {"count": len(ids_seen), "unique_ids": len(set(ids_seen)), "revisions_contiguous_per_project": rev_ok,
                         "by_origin": origins, "system_event_causes": sys_tools}
    with c.lock:
        r["push_notifications_received"] = len(c.notifications)

    # 10. render
    rn, rn_err, _ = c.tool(70, "render", {"project_id": A, "seconds": 5})
    r["render"] = rn

    c.p.stdin.close()
    c.p.wait(timeout=30)
    r["server_exit_code"] = c.p.returncode
    r["non_json_lines_on_stdout"] = c.bad_lines
    out = json.dumps(r, indent=2)
    print(out)
    if a.json:
        open(a.json, "w").write(out)


if __name__ == "__main__":
    main()
