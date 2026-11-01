#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Headless test client for the P3 browser-stream spike (stdlib only; libopus via ctypes).

Scenario: connect -> play from 0 for --play seconds -> seek to 30 s -> --after-seek seconds -> stop -> 1 s of silence check -> stats.
Measures: play->first audio, seek->first audio at the new place, stale-epoch packets, seq gaps, inter-arrival jitter,
simulated jitter-buffer underruns, decoded sample count, click alignment (timeline vs decoded content), server stats.
"""
import argparse
import base64
import ctypes
import hashlib
import json
import os
import socket
import statistics
import struct
import sys
import time

SEEK_TARGET = 60.0   # seconds; far from the play position so a seek is unmistakable
HDR = struct.Struct("<BBHHHIQQ")  # type flags stream epoch reserved seq sample_pos server_time_us (28 bytes)


class Ws:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(1)
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        assert b"101" in buf.split(b"\r\n")[0] and accept.encode() in buf, buf
        self.buf = b""

    def _need(self, n, deadline):
        while len(self.buf) < n:
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            self.s.settimeout(left)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return False
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        return True

    def recv(self, timeout):
        """-> (opcode, payload, t_monotonic) or None on timeout"""
        deadline = time.monotonic() + timeout
        if not self._need(2, deadline):
            return None
        n = self.buf[1] & 0x7F
        hl = 2
        if n == 126:
            if not self._need(4, deadline): return None
            n, hl = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            if not self._need(10, deadline): return None
            n, hl = struct.unpack(">Q", self.buf[2:10])[0], 10
        if not self._need(hl + n, deadline):
            return None
        op = self.buf[0] & 0xF
        payload = self.buf[hl:hl + n]
        self.buf = self.buf[hl + n:]
        return op, payload, time.monotonic()

    def send_text(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        hdr = bytes([0x81, 0x80 | len(data)]) + mask  # payloads < 126 bytes
        self.s.sendall(hdr + masked)


class Opus:
    def __init__(self):
        self.lib = ctypes.CDLL("libopus.so.0")
        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_decode_float.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_float), ctypes.c_int, ctypes.c_int]
        self.lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
        self.dec = None

    def reset(self):
        if self.dec:
            self.lib.opus_decoder_destroy(self.dec)
        err = ctypes.c_int(0)
        self.dec = self.lib.opus_decoder_create(48000, 2, ctypes.byref(err))
        assert err.value == 0

    def decode(self, data):
        out = (ctypes.c_float * (960 * 2 * 3))()
        n = self.lib.opus_decode_float(self.dec, data, len(data), out, 960 * 3, 0)
        return n, list(out[:n * 2]) if n > 0 else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--play", type=float, default=10.0)
    ap.add_argument("--after-seek", type=float, default=4.0)
    ap.add_argument("--stall", type=float, default=0.0, help="stop reading for N seconds after the first 2 s of playback (back-pressure test)")
    ap.add_argument("--json", help="write result json here")
    a = ap.parse_args()

    ws, opus = Ws(a.host, a.port), Opus()
    pk = []            # (t, seq, flags, epoch, pos, server_us, nbytes)
    epoch_now = None
    stale = {"after_seek": 0, "after_stop": 0}
    phase = "idle"
    events = {}
    click_err = []     # ms
    decoded_samples = 0
    last_click = {}
    cur_epoch_dec = None
    server_stats = None
    pos_samples = 0
    state_clicks = []
    transport_msgs = []

    def pump(seconds):
        nonlocal epoch_now, decoded_samples, cur_epoch_dec, server_stats
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            r = ws.recv(min(0.05, max(0.0, end - time.monotonic())))
            if r is None:
                continue
            op, pl, t = r
            if op == 1:
                m = json.loads(pl)
                if m["type"] == "transport":
                    transport_msgs.append((t, m))
                    epoch_now = m["epoch"]
                    events.setdefault(phase + "_transport_msg", t)
                    events["last_transport"] = m
                elif m["type"] == "stats":
                    server_stats = m
            elif op == 2:
                typ, flags, sid, ep, _, seq, pos, us = HDR.unpack(pl[:28])
                pk.append((t, seq, flags, ep, pos, us, len(pl)))
                if epoch_now is not None and ep < epoch_now:
                    key = "after_seek" if phase in ("seek", "post-seek") else "after_stop"
                    stale[key] += 1
                    continue                                   # a real client drops old-epoch packets
                if ep != cur_epoch_dec:
                    opus.reset()
                    cur_epoch_dec = ep
                n, pcm = opus.decode(pl[28:])
                decoded_samples += n
                # click detection on L (rising edge, 0.2 s refractory in timeline samples)
                for i in range(n):
                    if abs(pcm[2 * i]) > 0.08:
                        p = pos + i
                        if p - last_click.get(ep, -10**9) > 9600:
                            last_click[ep] = p
                            err = ((p + 12000) % 24000 - 12000) / 48.0   # ms vs the nearest 0.5 s grid point
                            state_clicks.append((phase, err))
                            click_err.append(err)

    # ---- play ----
    phase = "play"
    t_play = time.monotonic()
    ws.send_text({"cmd": "play", "position_s": 0})
    pump(0.0)
    while not [p for p in pk if p[0] > t_play] and time.monotonic() - t_play < 3:
        pump(0.01)
    first = [p for p in pk if p[0] > t_play]
    events["play_to_first_audio_ms"] = (first[0][0] - t_play) * 1000 if first else None
    if a.stall > 0:
        pump(2.0)
        time.sleep(a.stall)                     # socket not drained: server must drop old audio and flag a discontinuity
        pump(a.play - 2.0)
    else:
        pump(a.play)

    # ---- seek ----
    phase = "seek"
    n_before = len(pk)
    t_seek = time.monotonic()
    ws.send_text({"cmd": "seek", "position_s": SEEK_TARGET})
    seek_audio = None
    while time.monotonic() - t_seek < 3 and seek_audio is None:
        pump(0.01)
        for p in pk[n_before:]:
            if epoch_now is not None and p[3] == epoch_now and abs(p[4] / 48000.0 - SEEK_TARGET) < 0.05:
                seek_audio = p
                break
    events["seek_to_first_audio_at_target_ms"] = (seek_audio[0] - t_seek) * 1000 if seek_audio else None
    events["seek_first_packet_pos_error_ms"] = (seek_audio[4] / 48.0 - SEEK_TARGET * 1000.0) if seek_audio else None
    phase = "post-seek"
    pump(a.after_seek)
    ws.send_text({"cmd": "stats"})
    pump(0.3)
    stats_playing = server_stats

    # ---- stop ----
    phase = "stop"
    n_stop = len(pk)
    t_stop = time.monotonic()
    ws.send_text({"cmd": "stop"})
    pump(1.5)
    stop_msgs = [t for t, m in transport_msgs if m["state"] == "stopped" and t >= t_stop]
    events["stop_to_transport_message_ms"] = (stop_msgs[0] - t_stop) * 1000 if stop_msgs else None
    events["stop_to_last_audio_packet_ms"] = (pk[-1][0] - t_stop) * 1000 if len(pk) > n_stop else 0.0
    stop_epoch = epoch_now
    after_stop_new_epoch = [p for p in pk[n_stop:] if p[3] == stop_epoch]
    events["audio_packets_with_stop_epoch"] = len(after_stop_new_epoch)

    ws.send_text({"cmd": "stats"})
    pump(0.5)

    # ---- analysis ----
    ok = [p for p in pk]
    seqs = [p[1] for p in ok]
    gaps = sum(1 for x, y in zip(seqs, seqs[1:]) if y != x + 1)
    ia = [(y[0] - x[0]) * 1000 for x, y in zip(ok, ok[1:]) if y[1] == x[1] + 1]
    jit = [abs(v - 20.0) for v in ia]
    sim = {}
    for B in (100, 150, 200, 300):
        # one continuous play segment (first one): playout starts B ms after first arrival
        seg = [p for p in ok if p[3] == ok[0][3]]
        t0 = seg[0][0] + B / 1000.0
        arrived = {p[1]: p[0] for p in seg}
        under = 0
        for k in range(seg[0][1], seg[-1][1] + 1):
            due = t0 + (k - seg[0][1]) * 0.02
            if k not in arrived or arrived[k] > due:
                under += 1
        sim[f"buffer_{B}ms_underruns_of_{len(range(seg[0][1], seg[-1][1] + 1))}_frames"] = under
    ce = [e for _, e in state_clicks]
    deltas = {}
    for x, y in zip(ok, ok[1:]):
        if y[3] == x[3] and y[1] == x[1] + 1:
            deltas[y[4] - x[4]] = deltas.get(y[4] - x[4], 0) + 1
    top_deltas = sorted(deltas.items(), key=lambda kv: -kv[1])[:6]
    result = {
        "packets": len(pk), "seq_gaps": gaps,
        "discontinuity_flags": sum(1 for p in pk if p[2] & 1),
        "bytes_total": sum(p[6] for p in pk), "avg_kbps_while_playing": (sum(p[6] for p in pk) * 8 / 1000.0) / max(1e-9, (pk[-1][0] - pk[0][0])) if len(pk) > 1 else 0,
        "inter_arrival_ms": {"mean": statistics.mean(ia) if ia else None, "p99_abs_dev_from_20": sorted(jit)[int(len(jit) * 0.99)] if jit else None, "max_abs_dev_from_20": max(jit) if jit else None},
        "sample_pos_delta_between_consecutive_packets_top": top_deltas,
        "decoded_samples": decoded_samples, "decoded_seconds": decoded_samples / 48000.0,
        "stale_epoch_packets_received": stale,
        "events": {k: v for k, v in events.items() if not isinstance(v, (dict, float)) or k.endswith("_ms")},
        "jitter_buffer_sim": sim,
        "clicks_detected": len(ce),
        "click_vs_sample_pos_ms": {"mean": statistics.mean(ce) if ce else None, "min": min(ce) if ce else None, "max": max(ce) if ce else None,
                                   "stdev": statistics.pstdev(ce) if ce else None,
                                   "note": "includes Opus lookahead (~6.5 ms) and block-position quantisation"},
        "server_stats_while_playing": stats_playing,
        "server_stats_after_stop": server_stats,
    }
    out = json.dumps(result, indent=2)
    print(out)
    if a.json:
        open(a.json, "w").write(out)


if __name__ == "__main__":
    main()
