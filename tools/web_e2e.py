#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Live checks of the web side of a running `rovox serve --audio browser` with a plain Python client (stdlib only).
#
#   stream:  tools/web_e2e.py --url http://127.0.0.1:4810 --token T stream --project mukola --seconds 20
#            opens /ws like the browser does, plays, and reports audio packets, sequence gaps, inter-arrival jitter, a 150 ms jitter-buffer
#            simulation (underruns), position and meter frames.
#   agent:   tools/web_e2e.py --url http://127.0.0.1:4810 --token T agent --project mukola
#            talks MCP over HTTP like Claude Code: changes a fader, splits an item, adds an effect. A web UI open on the project must show
#            the changes at once in agent-cyan, with "Claude" as the origin.
#   watch:   tools/web_e2e.py ... watch --project mukola --seconds 30     prints every event the browser would get.
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.parse
import urllib.request


class Ws:
    def __init__(self, url, token):
        u = urllib.parse.urlparse(url)
        self.sock = socket.create_connection((u.hostname, u.port or 80), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET /ws HTTP/1.1\r\nHost: {u.hostname}:{u.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: http://{u.hostname}:{u.port}\r\n"
               + (f"Authorization: Bearer {token}\r\n" if token else "") + "\r\n")
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.sock.recv(1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError(head.decode(errors="replace"))

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self, timeout=5.0):
        self.sock.settimeout(timeout)
        h = self._read(2)
        op, n = h[0] & 0xF, h[1] & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._read(8))[0]
        payload = self._read(n)
        return op, payload

    def cmd(self, name, params=None, project=None, wait=10):
        cid = f"c_{time.time_ns()}"
        self.send({"t": "cmd", "id": cid, "name": name, "params": params or {}, "project_id": project})
        t0 = time.time()
        while time.time() - t0 < wait:
            op, payload = self.recv(wait)
            if op == 1:
                m = json.loads(payload)
                if m.get("t") == "result" and m.get("id") == cid:
                    return m
        raise TimeoutError(name)


def find_project(ws, name):
    r = ws.cmd("project.list")
    for p in r["data"]["projects"]:
        if name in (p["project_id"], p["name"]):
            return p["project_id"]
    raise SystemExit(f"no open project {name}; open it first (project.open)")


def stream(args):
    ws = Ws(args.url, args.token)
    ws.send({"t": "hello", "client": "web_e2e/0"})
    pid = find_project(ws, args.project)
    ws.send({"t": "sub", "project_id": pid, "channels": ["state", "transport", "meters"]})
    ws.send({"t": "audio", "on": True})
    ws.cmd("transport.set", {"state": "playing", "position_s": args.start}, pid)
    t0 = time.time()
    arrivals, seqs, positions, meters, epochs = [], [], 0, 0, set()
    last_meter = None
    while time.time() - t0 < args.seconds:
        try:
            op, payload = ws.recv(2)
        except socket.timeout:
            continue
        now = time.time()
        if op != 2 or not payload:
            continue
        kind = payload[0]
        if kind == 1:
            seq = struct.unpack_from("<I", payload, 8)[0]
            epochs.add(struct.unpack_from("<H", payload, 4)[0])
            arrivals.append(now)
            seqs.append(seq)
        elif kind == 3:
            positions += 1
        elif kind == 2:
            meters += 1
            cnt = struct.unpack_from("<H", payload, 2)[0]
            last_meter = [struct.unpack_from("<Iff", payload, 4 + 12 * i) for i in range(cnt)]
    ws.cmd("transport.set", {"state": "stopped"}, pid)
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    dev = sorted(abs((b - a) - 0.020) * 1000 for a, b in zip(arrivals, arrivals[1:]))
    # jitter buffer simulation: start when `target` ms are buffered, drain in real time
    def simulate(target_ms):
        buffered, started, under, last = 0.0, False, 0, arrivals[0] if arrivals else 0
        events = sorted([(a, "p") for a in arrivals])
        for when, _ in events:
            if started:
                buffered -= (when - last) * 1000
                if buffered < 0:
                    under += 1
                    started, buffered = False, 0.0
            last = when
            buffered += 20
            if not started and buffered >= target_ms:
                started = True
        return under
    print(json.dumps({
        "seconds": args.seconds, "audio_packets": len(arrivals), "expected": int(args.seconds * 50), "sequence_gaps": gaps,
        "epochs": sorted(epochs), "position_frames": positions, "meter_frames": meters,
        "interarrival_dev_ms_p50": round(dev[len(dev) // 2], 2) if dev else None,
        "interarrival_dev_ms_p99": round(dev[int(len(dev) * 0.99)], 2) if dev else None,
        "interarrival_dev_ms_max": round(dev[-1], 2) if dev else None,
        "simulated_underruns": {str(t): simulate(t) for t in (100, 150, 200)},
        "last_meter_entries(index,L dB,R dB)": [[int(i), round(l, 1), round(r, 1)] for i, l, r in (last_meter or [])][:4],
    }, indent=2))


class Mcp:
    def __init__(self, url, token):
        self.url, self.token, self.session, self.n = url.rstrip("/") + "/mcp", token, None, 0

    def rpc(self, method, params=None, notify=False):
        self.n += 1
        msg = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notify:
            msg["id"] = self.n
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        with urllib.request.urlopen(urllib.request.Request(self.url, json.dumps(msg).encode(), headers), timeout=60) as r:
            self.session = r.headers.get("Mcp-Session-Id") or self.session
            body = r.read().decode()
        if notify or not body.strip():
            return None
        for line in ([body] if body.lstrip().startswith("{") else [l[5:] for l in body.splitlines() if l.startswith("data:")]):
            m = json.loads(line)
            if m.get("id") == self.n:
                return m
        return None

    def tool(self, name, **args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        return r["result"]["structuredContent"]


def agent(args):
    mcp = Mcp(args.url, args.token)
    mcp.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "Claude", "version": "e2e"}})
    mcp.rpc("notifications/initialized", notify=True)
    pid = None
    for p in mcp.tool("project.list")["data"]["projects"]:
        if args.project in (p["project_id"], p["name"]):
            pid = p["project_id"]
    if not pid:
        raise SystemExit("no such open project")
    tracks = mcp.tool("track.list", project_id=pid)["data"]["tracks"]
    print("tracks:", [t["name"] for t in tracks])
    t = tracks[args.track]
    steps = [
        ("track.update", dict(project_id=pid, track_id=t["track_id"], volume_db=-6.5, pan=-0.2, undo_label="e2e balance")),
    ]
    items = mcp.tool("item.list", project_id=pid, track_id=t["track_id"])["data"]["items"]
    if items:
        it = items[0]
        steps.append(("item.split", dict(project_id=pid, item_id=it["item_id"], at_s=it["start_s"] + min(10.0, it["length_s"] / 2))))
    steps.append(("fx.add", dict(project_id=pid, track_id=t["track_id"], builtin="reverb")))
    for name, a in steps:
        r = mcp.tool(name, **a)
        print(name, "->", "ok" if r.get("ok") else r.get("error"), "revision", r.get("revision"))
        time.sleep(args.pause)


def watch(args):
    ws = Ws(args.url, args.token)
    ws.send({"t": "hello", "client": "web_e2e/0"})
    pid = find_project(ws, args.project)
    ws.send({"t": "sub", "project_id": pid})
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        try:
            op, payload = ws.recv(2)
        except socket.timeout:
            continue
        if op == 1:
            m = json.loads(payload)
            if m.get("t") == "event":
                print(json.dumps({k: m[k] for k in m if k in ("type", "revision", "origin", "undo_label", "changes")}))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:4810")
    ap.add_argument("--token", default=os.environ.get("ROVOX_TOKEN", ""))
    sub = ap.add_subparsers(dest="mode", required=True)
    s = sub.add_parser("stream")
    s.add_argument("--project", required=True)
    s.add_argument("--seconds", type=float, default=20)
    s.add_argument("--start", type=float, default=0)
    s.set_defaults(fn=stream)
    a = sub.add_parser("agent")
    a.add_argument("--project", required=True)
    a.add_argument("--track", type=int, default=0)
    a.add_argument("--pause", type=float, default=1.5)
    a.set_defaults(fn=agent)
    w = sub.add_parser("watch")
    w.add_argument("--project", required=True)
    w.add_argument("--seconds", type=float, default=30)
    w.set_defaults(fn=watch)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
