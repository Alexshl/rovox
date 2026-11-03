#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Numerical comparison of two renders (reference = REAPER, test = Rovox) of the same input.
#   alignment by cross-correlation with sub-sample (parabolic) refinement, null residual (RMS and peak, dBFS), 1/3-octave band level
#   differences, integrated LUFS (ITU-R BS.1770-4, 48 kHz) and true peak (4x oversampled), 1 ms RMS envelope difference split into
#   attack / release / steady regions (regions follow the level steps of the INPUT signal), RT60 per octave band from an impulse
#   response (Schroeder). A gain-matched residual is reported for diagnosis only; no pass/fail ever uses it.
# Command line: compare.py REF.wav TEST.wav [--input IN.wav] [--json out.json] [--png out.png]
import argparse
import json
import sys

import numpy as np
import soundfile as sf
from scipy import optimize, signal

EPS = 1e-12
THIRD_OCT = [20 * 2 ** (i / 3) for i in range(0, 31)]       # nominal-ish centres 20 Hz .. 20 kHz
THIRD_OCT = [f for f in [25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150,
                         4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000]]


def dbfs(x):
    return 20 * np.log10(max(float(x), EPS))


def load(path):
    x, sr = sf.read(path, dtype="float64", always_2d=True)
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2], sr


# ---------------------------------------------------------------------------------------------------------------- alignment
def estimate_lag(ref, test, max_lag):
    """Delay of the test in samples (float, > 0: test is later): test[n + lag] ~ ref[n]. Smallest |lag| among near-equal correlation maxima (periodic signals)."""
    a, b = ref.sum(axis=1), test.sum(axis=1)
    n = len(a) + len(b)
    nfft = 1 << (n - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(a, nfft) * np.conj(np.fft.rfft(b, nfft)), nfft)       # c[k] = sum a[m+k] b[m]  (lag k: a is later)
    lags = np.arange(-max_lag, max_lag + 1)
    vals = c[lags % nfft]
    peak = vals.max()
    if peak <= 0:
        return 0.0, 0.0
    cands = np.where(vals >= 0.999 * peak)[0]
    i = cands[np.argmin(np.abs(lags[cands]))]
    frac = 0.0
    if 0 < i < len(vals) - 1:
        y0, y1, y2 = vals[i - 1], vals[i], vals[i + 1]
        den = y0 - 2 * y1 + y2
        if den < 0:
            frac = 0.5 * (y0 - y2) / den
    norm = np.sqrt(np.sum(a ** 2) * np.sum(b ** 2)) + EPS
    return -float(lags[i] + frac), float(vals[i] / norm)


def shift(x, lag):
    """y[n] = x[n + lag] for fractional lag (zero filled): integer part by slicing, fraction by an FFT phase ramp."""
    n = len(x)
    k = int(np.floor(lag))
    f = lag - k
    y = np.zeros_like(x)
    if k >= 0:
        y[: n - k] = x[k:] if k < n else 0
    else:
        y[-k:] = x[: n + k]
    if abs(f) > 1e-3:
        nfft = 1 << (n + 64 - 1).bit_length()
        w = 2 * np.pi * np.fft.rfftfreq(nfft)
        y = np.fft.irfft(np.fft.rfft(y, nfft, axis=0) * np.exp(1j * w * f)[:, None], nfft, axis=0)[:n]
    return y


# ---------------------------------------------------------------------------------------------------------------- loudness
def _k_weight(x, sr):
    if sr != 48000:
        raise ValueError("BS.1770 coefficients are for 48 kHz")
    s1 = signal.lfilter([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585], x, axis=0)
    return signal.lfilter([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621], s1, axis=0)


def lufs(x, sr=48000):
    """Integrated loudness, BS.1770-4 with the -70 LUFS absolute and -10 LU relative gates; None if shorter than one block."""
    y = _k_weight(x, sr)
    blk, hop = int(0.4 * sr), int(0.1 * sr)
    if len(y) < blk:
        return None
    cs = np.vstack([np.zeros((1, y.shape[1])), np.cumsum(y ** 2, axis=0)])
    starts = np.arange(0, len(y) - blk + 1, hop)
    z = (cs[starts + blk] - cs[starts]).sum(axis=1) / blk            # channel weights 1.0 (L, R)
    l = -0.691 + 10 * np.log10(np.maximum(z, 1e-20))
    g1 = z[l > -70]
    if len(g1) == 0:
        return None
    rel = -0.691 + 10 * np.log10(g1.mean()) - 10
    g2 = z[l > rel]
    if len(g2) == 0:
        return None
    return float(-0.691 + 10 * np.log10(g2.mean()))


def true_peak_db(x):
    return dbfs(np.max(np.abs(signal.resample_poly(x, 4, 1, axis=0))))


# ---------------------------------------------------------------------------------------------------------------- spectrum
def third_octave_levels(x, sr=48000):
    """Level (dB, relative to full scale) per 1/3-octave band from the averaged Welch PSD of both channels."""
    nper = min(16384, len(x))
    f, p = signal.welch(x, sr, window="hann", nperseg=nper, noverlap=nper // 2, axis=0)
    p = p.mean(axis=1)
    df = f[1] - f[0]
    out = []
    for fc in THIRD_OCT:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        m = (f >= lo) & (f < hi)
        out.append(10 * np.log10(max(p[m].sum() * df, 1e-20)) if m.any() else -200.0)
    return np.array(out)


def band_diffs(ref, test, sr, span_db=40.0):
    lr, lt = third_octave_levels(ref, sr), third_octave_levels(test, sr)
    keep = lr >= lr.max() - span_db            # only bands where the reference has energy
    d = np.where(keep, lt - lr, np.nan)
    return {"centres_hz": THIRD_OCT, "ref_db": lr.round(2).tolist(), "test_db": lt.round(2).tolist(),
            "diff_db": [None if np.isnan(v) else round(float(v), 3) for v in d],
            "max_abs_db": float(np.nanmax(np.abs(d))) if keep.any() else None}


# ---------------------------------------------------------------------------------------------------------------- envelope
def envelope_db(x, sr=48000, win_ms=1.0):
    n = int(sr * win_ms / 1000)
    p = (x ** 2).sum(axis=1)
    m = len(p) // n
    e = p[: m * n].reshape(m, n).mean(axis=1)
    return 10 * np.log10(np.maximum(e, 1e-20))


def level_steps(inp_env, step_db=10.0, look=4, quiet=3):
    """(index_ms, +1/-1) of upward / downward level steps in a 1 ms envelope; events closer than 50 ms are merged."""
    ev, last = [], -1000
    for i in range(len(inp_env) - look):
        d = inp_env[i + look] - inp_env[i]
        if abs(d) >= step_db and i - last > 50:
            ev.append((i, 1 if d > 0 else -1))
            last = i
    return ev


def envelope_report(ref, test, inp, sr, floor_db=-90.0, attack_ms=150, release_ms=500):
    er, et = envelope_db(ref, sr), envelope_db(test, sr)
    m = min(len(er), len(et))
    er, et = er[:m], et[:m]
    diff = et - er
    valid = er > floor_db
    region = np.zeros(m, dtype=int)                  # 0 steady, 1 attack, 2 release
    steps = []
    if inp is not None:
        ei = envelope_db(inp, sr)
        steps = level_steps(ei)
        for i, sgn in steps:
            lo, hi = max(i - 2, 0), min(i + (attack_ms if sgn > 0 else release_ms), m)
            region[lo:hi] = 1 if sgn > 0 else 2
    out = {"steps_ms": [[int(i), int(s)] for i, s in steps]}
    for name, code in (("steady", 0), ("attack", 1), ("release", 2)):
        sel = valid & (region == code)
        out[name] = ({"max_abs_db": float(np.abs(diff[sel]).max()), "rms_db": float(np.sqrt(np.mean(diff[sel] ** 2))), "n": int(sel.sum())}
                     if sel.any() else None)
    allv = [v["max_abs_db"] for k, v in out.items() if k in ("steady", "attack", "release") and v]
    out["max_abs_db"] = max(allv) if allv else None
    out["_arrays"] = (er, et, diff, region)
    return out


# ---------------------------------------------------------------------------------------------------------------- RT60
def rt60_bands(x, sr=48000, centres=(125, 250, 500, 1000, 2000, 4000)):
    """RT60 (s) per octave band from an impulse response: Schroeder integral, T20 (-5..-25 dB) extrapolated to 60 dB; None if not found."""
    mono = x.mean(axis=1)
    out = {}
    for fc in centres:
        sos = signal.butter(4, [fc / 2 ** 0.5, min(fc * 2 ** 0.5, sr / 2 * 0.99)], btype="band", fs=sr, output="sos")
        y = signal.sosfilt(sos, mono)
        e = np.cumsum((y ** 2)[::-1])[::-1]
        if e[0] <= 0:
            out[str(fc)] = None
            continue
        edc = 10 * np.log10(np.maximum(e / e[0], 1e-20))
        i5, i25 = np.argmax(edc <= -5), np.argmax(edc <= -25)
        if i25 <= i5 or edc[i25] > -25:
            out[str(fc)] = None
            continue
        t = np.arange(i5, i25) / sr
        slope = np.polyfit(t, edc[i5:i25], 1)[0]
        out[str(fc)] = float(-60.0 / slope) if slope < 0 else None
    return out


# ---------------------------------------------------------------------------------------------------------------- compare
def compare(ref_path, test_path, input_path=None, max_lag_s=0.05, want_rt60=False):
    ref, sr = load(ref_path)
    test, sr2 = load(test_path)
    if sr != sr2:
        raise ValueError("sample rates differ: %s vs %s" % (sr, sr2))
    inp = load(input_path)[0] if input_path else None
    n = max(len(ref), len(test))
    pad = lambda x: np.vstack([x, np.zeros((n - len(x), 2))])
    ref, test = pad(ref), pad(test)
    lag, corr = estimate_lag(ref, test, int(max_lag_s * sr))
    # sub-sample refinement: minimise the residual energy around the correlation peak (parabolic interpolation alone is biased)
    base = round(lag)
    cost = lambda d: float(np.sum((ref - shift(test, base + d)) ** 2))
    opt = optimize.minimize_scalar(cost, bounds=(-1.0, 1.0), method="bounded", options={"xatol": 1e-4})
    lag = base + float(opt.x) if opt.fun < cost(lag - base) else lag
    test_al = shift(test, lag)
    res = ref - test_al
    rep = {"sample_rate": sr, "frames": {"ref": int(len(ref)), "test": int(len(test))}, "lag_samples": round(lag, 4), "correlation": round(corr, 6),
           "null": {"rms_dbfs": dbfs(np.sqrt(np.mean(res ** 2))), "peak_dbfs": dbfs(np.abs(res).max()),
                    "ref_rms_dbfs": dbfs(np.sqrt(np.mean(ref ** 2))), "test_rms_dbfs": dbfs(np.sqrt(np.mean(test_al ** 2)))}}
    # diagnosis only: how far a plain gain would take the null
    g = float((ref * test_al).sum() / ((test_al ** 2).sum() + EPS))
    rep["gain_matched_info_only"] = {"gain_db": dbfs(abs(g)) if g != 0 else None,
                                     "null_rms_dbfs": dbfs(np.sqrt(np.mean((ref - g * test_al) ** 2)))}
    lr, lt = lufs(ref, sr), lufs(test_al, sr)
    rep["lufs"] = {"ref": lr, "test": lt, "diff": None if lr is None or lt is None else lt - lr}
    tr, tt = true_peak_db(ref), true_peak_db(test_al)
    rep["true_peak_db"] = {"ref": tr, "test": tt, "diff": tt - tr}
    rep["sample_peak_db"] = {"ref": dbfs(np.abs(ref).max()), "test": dbfs(np.abs(test_al).max())}
    rep["bands"] = band_diffs(ref, test_al, sr)
    env = envelope_report(ref, test_al, inp, sr)
    rep["_env_arrays"] = env.pop("_arrays")
    rep["envelope"] = env
    if want_rt60:
        a, b = rt60_bands(ref, sr), rt60_bands(test_al, sr)
        rep["rt60"] = {k: {"ref": a[k], "test": b[k], "rel_diff": (None if a[k] is None or b[k] is None else (b[k] - a[k]) / a[k])} for k in a}
    rep["_audio"] = (ref, test_al, res, sr)
    return rep


def check(rep, th):
    """List of (name, value, limit, passed). Thresholds: null_db (<=), lufs_db, peak_db, bands_db, env_db, rt60_rel (absolute differences)."""
    out = []

    def add(name, value, limit, upper=True):
        ok = value is not None and (abs(value) <= limit if upper == "abs" else value <= limit)
        out.append((name, value, limit, bool(ok)))

    if "null_db" in th:
        add("null_rms_dbfs", rep["null"]["rms_dbfs"], th["null_db"])
    if "lufs_db" in th:
        add("lufs_diff", rep["lufs"]["diff"], th["lufs_db"], "abs")
    if "peak_db" in th:
        add("true_peak_diff", rep["true_peak_db"]["diff"], th["peak_db"], "abs")
    if "bands_db" in th:
        add("band_max_abs_db", rep["bands"]["max_abs_db"], th["bands_db"])
    if "env_db" in th:
        add("env_max_abs_db", rep["envelope"]["max_abs_db"], th["env_db"])
    if "rt60_rel" in th and "rt60" in rep:
        vals = [abs(v["rel_diff"]) for v in rep["rt60"].values() if v["rel_diff"] is not None]
        add("rt60_max_rel", max(vals) if vals else None, th["rt60_rel"])
    return out


def plot(rep, path, title=""):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    ref, test, res, sr = rep["_audio"]
    er, et, diff, region = rep["_env_arrays"]
    fig, ax = plt.subplots(4, 1, figsize=(12, 13))
    t = np.arange(len(ref)) / sr
    ax[0].plot(t, ref[:, 0], lw=0.6, label="REAPER")
    ax[0].plot(t, test[:, 0], lw=0.6, alpha=0.7, label="Rovox")
    ax[0].plot(t, res[:, 0], lw=0.6, color="r", label="residual")
    ax[0].set_title("%s  null %.1f dBFS rms / %.1f peak, lag %.3f smp" % (title, rep["null"]["rms_dbfs"], rep["null"]["peak_dbfs"], rep["lag_samples"]))
    ax[0].legend(loc="upper right"); ax[0].set_xlabel("s")
    f, pr = signal.welch(ref[:, 0], sr, nperseg=min(8192, len(ref)))
    _, pt = signal.welch(test[:, 0], sr, nperseg=min(8192, len(ref)))
    ax[1].semilogx(f[1:], 10 * np.log10(pr[1:] + 1e-20), label="REAPER")
    ax[1].semilogx(f[1:], 10 * np.log10(pt[1:] + 1e-20), label="Rovox", alpha=0.7)
    ax[1].set_xlim(20, sr / 2); ax[1].legend(); ax[1].set_ylabel("PSD dB"); ax[1].grid(True, which="both", alpha=0.3)
    d = np.array([np.nan if v is None else v for v in rep["bands"]["diff_db"]])
    ax[2].bar(range(len(d)), np.nan_to_num(d))
    ax[2].set_xticks(range(len(d))); ax[2].set_xticklabels([str(int(c)) if c >= 100 else str(c) for c in THIRD_OCT], rotation=60, fontsize=7)
    ax[2].set_ylabel("Rovox - REAPER dB"); ax[2].set_title("1/3-octave band difference"); ax[2].grid(True, alpha=0.3)
    te = np.arange(len(er)) / 1000.0
    ax[3].plot(te, er, lw=0.7, label="REAPER env"); ax[3].plot(te, et, lw=0.7, alpha=0.7, label="Rovox env")
    ax[3].set_ylim(-100, 5); ax[3].set_ylabel("1 ms RMS dB"); ax[3].set_xlabel("s")
    ax3 = ax[3].twinx(); ax3.plot(te, diff, color="r", lw=0.5, alpha=0.6); ax3.set_ylim(-6, 6); ax3.set_ylabel("diff dB", color="r")
    ax[3].legend(loc="upper right")
    fig.tight_layout(); fig.savefig(path, dpi=80); plt.close(fig)


def clean(rep):
    return {k: v for k, v in rep.items() if not k.startswith("_")}


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ref"); ap.add_argument("test")
    ap.add_argument("--input"); ap.add_argument("--json"); ap.add_argument("--png")
    ap.add_argument("--max-lag", type=float, default=0.05); ap.add_argument("--rt60", action="store_true")
    a = ap.parse_args()
    r = compare(a.ref, a.test, a.input, a.max_lag, a.rt60)
    if a.png:
        plot(r, a.png)
    txt = json.dumps(clean(r), indent=1)
    if a.json:
        open(a.json, "w").write(txt)
    else:
        print(txt)
