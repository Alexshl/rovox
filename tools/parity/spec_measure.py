#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Black-box measurement helpers for the REAPER behaviour specs (docs/parity/spec-*.md): render generated test signals through REAPER
# (reaper_driver.run_job, new tab, nothing of the user's is touched) and return the output as numpy arrays.
import hashlib
import json
import os
import sys

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import reaper_driver  # noqa: E402

SR = 48000
OUT = os.path.expanduser("~/.rovox-parity/spec")


def render(name, x, fx=None, params=(), config=None, item_info=None, take_info=None, tail_ms=0, reuse=True, scan=None, srate=None):
    """x: (frames,) or (frames, ch) float array at 48 kHz. Returns (y (frames, 2), log)."""
    os.makedirs(OUT, exist_ok=True)
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1:
        x = x[:, None]
    h = hashlib.sha1(x.tobytes()).hexdigest()[:10] + ("" if not srate else "_%d" % srate)
    sig = os.path.join(OUT, "sig_%s.wav" % h)
    if not os.path.exists(sig):
        sf.write(sig, x, srate or SR, subtype="FLOAT")
    work = os.path.join(OUT, name)
    wav = os.path.join(work, name + ".wav")
    logp = os.path.join(work, name + ".reaper.json")
    if reuse and os.path.exists(wav) and os.path.exists(logp):
        y, _ = sf.read(wav, always_2d=True)
        return y, json.load(open(logp, encoding="utf-8", errors="replace"))
    job = {"name": name, "out_dir": work, "signal": sig, "label": "spec", "tail_ms": tail_ms,
           "fx": fx, "params": [{"name": k, "value": v} for k, v in params], "config": config or {},
           "item_info": item_info or {}, "take_info": take_info or {}, "scan": scan}
    if srate:
        job["srate"] = srate
    r = reaper_driver.run_job(job)
    y, _ = sf.read(r["wav"], always_2d=True)
    return y, r["log"]


def impulse(n=SR, at=SR // 10, amp=1.0):
    x = np.zeros(n)
    x[at] = amp
    return x


def mag_db(h, freqs, sr=SR):
    H = np.fft.rfft(h, 1 << 18)
    f = np.fft.rfftfreq(1 << 18, 1.0 / sr)
    return np.interp(freqs, f, 20 * np.log10(np.abs(H) + 1e-30))
