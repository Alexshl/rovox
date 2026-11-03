#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Deterministic test signals for the REAPER/Rovox FX parity harness: 48 kHz float32 WAV, generated on the fly into a work directory.
#   impulse, sine_{100,1k,10k}_{m20,m6}, sweep, pink, bursts, dc_gate, dc_limit, silence
# `channels=2` gives a stereo version (right = left at -3 dB, or an independent noise for `pink`), `channels=1` is mono.
import argparse
import os

import numpy as np
import soundfile as sf

SR = 48000


def db(x):
    return 10.0 ** (x / 20.0)


def _sine(freq, level_db, seconds=3.0):
    t = np.arange(int(seconds * SR)) / SR
    return db(level_db) * np.sin(2 * np.pi * freq * t)


def _pink(seed, seconds=5.0, rms_db=-20.0):
    n = int(seconds * SR)
    spec = np.fft.rfft(np.random.default_rng(seed).standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    f[0] = f[1]
    x = np.fft.irfft(spec / np.sqrt(f), n)
    return x * (db(rms_db) / np.sqrt(np.mean(x ** 2)))


def _sweep(seconds=5.0, level_db=-6.0, f0=20.0, f1=20000.0):
    t = np.arange(int(seconds * SR)) / SR
    k = np.log(f1 / f0)
    return db(level_db) * np.sin(2 * np.pi * f0 * seconds / k * (np.exp(t / seconds * k) - 1.0))


def _bursts():
    """1 kHz sine, 0.5 s segments alternating -40 dB / -6 dB / -40 / -6 / -40 (attack at 0.5 s, release at 1.0 s ...)."""
    t = np.arange(int(0.5 * SR)) / SR
    parts = []
    for lvl in (-40, -6, -40, -6, -40):
        parts.append(db(lvl) * np.sin(2 * np.pi * 1000.0 * (t + 0.0)))
    return np.concatenate(parts)


def _dc_gate():
    """DC bursts for gate timing (spec-reagate.md v2): -10 dBFS bursts of 2/20/200 ms on -40 dBFS (1 s apart), then a staircase
    -19/-40/-17.5/-40/-15/-17.5/-18.5/-19.5/-21/-23/-25/-27/-40 (100 ms each) and a re-trigger -10 (100 ms) / -40 (30 ms) / -10 (20 ms)."""
    parts = [np.full(SR // 2, db(-40))]
    for ms in (2, 20, 200):
        parts += [np.full(ms * SR // 1000, db(-10)), np.full(SR, db(-40))]
    for lvl in (-19, -40, -17.5, -40, -15, -17.5, -18.5, -19.5, -21, -23, -25, -27, -40):
        parts.append(np.full(SR // 10, db(lvl)))
    parts += [np.full(SR // 10, db(-10)), np.full(3 * SR // 100, db(-40)), np.full(SR // 50, db(-10)), np.full(3 * SR // 10, db(-40))]
    return np.concatenate(parts)


def _dc_limit():
    """DC overshoots for limiter release (spec-realimit.md v2), floor -40 dBFS: -3 dBFS steps of 10/30/100/300/1000 ms (1.6 s gaps),
    10 ms pairs with 5/10.5/15 ms gaps, trains (2 s) of period 20 ms/duty 50 % and 100 ms/25 %, then 3 s of pink noise peaking at 0 dBFS."""
    lo, hi = db(-40), db(-3)
    gap = np.full(int(1.6 * SR), lo)
    parts = [np.full(SR // 2, lo)]
    for ms in (10, 30, 100, 300, 1000):
        parts += [np.full(ms * SR // 1000, hi), gap]
    for g in (5, 10.5, 15):
        parts += [np.full(480, hi), np.full(int(g * SR / 1000), lo), np.full(480, hi), gap]
    t = np.arange(2 * SR)
    for per, duty in ((960, 0.5), (4800, 0.25)):
        parts += [np.where((t % per) < int(duty * per), hi, lo), gap]
    p = _pink(77, seconds=3.0)
    parts += [p / np.max(np.abs(p)), gap]
    return np.concatenate(parts)


def _impulse():
    x = np.zeros(SR)
    x[SR // 10] = 1.0
    return x


NAMES = (["impulse", "sweep", "pink", "bursts", "dc_gate", "dc_limit"]
         + ["sine_%s_%s" % (f, l) for f in ("100", "1k", "10k") for l in ("m20", "m6")])


def make(name, channels=1):
    """Float32 array (frames, channels)."""
    freqs = {"100": 100.0, "1k": 1000.0, "10k": 10000.0}
    if name == "impulse":
        left = right = _impulse()
    elif name == "sweep":
        left = _sweep()
        right = None
    elif name == "bursts":
        left = _bursts()
        right = None
    elif name == "dc_gate":
        left = _dc_gate()
        right = None
    elif name == "dc_limit":
        left = _dc_limit()
        right = None
    elif name == "pink":
        left = _pink(1234)
        right = _pink(4321) if channels == 2 else None
    elif name.startswith("sine_"):
        _, f, lvl = name.split("_")
        left = _sine(freqs[f], -float(lvl[1:]))
        right = None
    else:
        raise ValueError("unknown signal " + name)
    if channels == 1:
        out = left[:, None]
    else:
        if right is None:
            right = left * db(-3.0)
        out = np.stack([left, right], axis=1)
    return out.astype(np.float32)


def write(name, channels, directory):
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, "%s_%dch.wav" % (name, channels))
    if not os.path.exists(path):
        sf.write(path, make(name, channels), SR, subtype="FLOAT")
    return path


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, help="work directory")
    ap.add_argument("--channels", type=int, default=1, choices=(1, 2))
    ap.add_argument("names", nargs="*", default=NAMES)
    a = ap.parse_args()
    for n in a.names:
        print(write(n, a.channels, a.out))
