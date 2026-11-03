#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Deterministic test signals for the REAPER/Rovox FX parity harness: 48 kHz float32 WAV, generated on the fly into a work directory.
#   impulse, sine_{100,1k,10k}_{m20,m6}, sweep, pink, bursts, silence
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


def _impulse():
    x = np.zeros(SR)
    x[SR // 10] = 1.0
    return x


NAMES = (["impulse", "sweep", "pink", "bursts"]
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
