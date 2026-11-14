# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Audio analysis with the standard library only: tempo estimation from a WAV (onset energy + inter-onset histogram), and the
`analysis.tempo_map` tool that renders a track or the master through the core, downloads the WAV over HTTP and estimates its tempo."""
import array
import math
import urllib.parse
import urllib.request
import wave

MIN_BPM, MAX_BPM = 60.0, 180.0
ACCENT_RATIO = 1.5      # on-grid vs off-grid mean onset energy that makes the half tempo the pulse


def _frame_energies(path, hop_s=0.005):
    """Mean-square energy per hop of the mono mix of a PCM WAV. Returns (energies, hop_seconds)."""
    with wave.open(path, "rb") as w:
        ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        hop = max(1, int(rate * hop_s))
        energies = []
        while True:
            raw = w.readframes(hop)
            n = len(raw) // (width * ch)
            if n == 0:
                break
            if width == 2:
                vals = array.array("h")
                vals.frombytes(raw[:n * ch * 2])
                scale = 32768.0
            elif width == 1:
                vals = [b - 128 for b in raw[:n * ch]]
                scale = 128.0
            elif width in (3, 4):
                vals = [int.from_bytes(raw[i:i + width], "little", signed=True) for i in range(0, n * ch * width, width)]
                scale = float(1 << (8 * width - 1))
            else:
                raise ValueError("unsupported WAV sample width: %d" % width)
            total = 0.0
            for i in range(0, n * ch, ch):
                m = sum(vals[i:i + ch]) / ch / scale
                total += m * m
            energies.append(total / n)
        return energies, hop / float(rate)


def _onsets(energies, hop_s):
    """Onsets as (time, strength) pairs: peaks of the positive difference of the log energy. The energy is floored relative to the loudest frame
    (so a digital-silence lead-in does not stretch the flux range) and the threshold comes from a percentile of the flux, not from its maximum.
    The strength is the peak energy right after the onset (used to tell accented pulses from weak ones)."""
    if len(energies) < 3:
        return []
    floor = max(1e-4 * max(energies), 1e-12)
    logs = [math.log(max(e, floor)) for e in energies]
    flux = [0.0] + [max(0.0, logs[i] - logs[i - 1]) for i in range(1, len(logs))]
    positive = sorted(f for f in flux if f > 1e-6)
    if not positive:
        return []
    thr = max(0.35 * positive[int(0.9 * (len(positive) - 1))], 0.5)
    out, i, guard = [], 1, int(0.05 / hop_s)
    while i < len(flux) - 1:
        if flux[i] >= thr and flux[i] >= flux[i - 1] and flux[i] > flux[i + 1]:
            out.append((i * hop_s, max(energies[i:i + 3])))
            i += max(1, guard)
        else:
            i += 1
    return out


def _explained(iois, period):
    return [d / round(d / period) for d in iois if 1 <= round(d / period) <= 8 and abs(d - round(d / period) * period) <= 0.02 * round(d / period) * period + 0.006]


def _accent_ratio(peaks, period):
    """Mean strength of the onsets on a grid of `period` anchored at the strongest onset, divided by that of the onsets off the grid (None when one side is empty)."""
    anchor = max(peaks, key=lambda p: p[1])[0]
    tol = 0.04 * period + 0.008
    on, off = [], []
    for t, s in peaks:
        phase = (t - anchor) / period
        (on if abs(phase - round(phase)) * period <= tol else off).append(s)
    if not on or not off:
        return None
    return (sum(on) / len(on)) / max(sum(off) / len(off), 1e-12)


def _estimate(peaks):
    onsets = [t for t, _ in peaks]
    iois = []
    for a in range(len(onsets)):
        for b in range(a + 1, min(a + 9, len(onsets))):
            iois.append(onsets[b] - onsets[a])
    if len(onsets) < 3:
        return None
    best, best_score = None, 0.0
    bpm = MIN_BPM
    while bpm <= MAX_BPM:
        period = 60.0 / bpm
        score = 0.0
        for d in iois:
            k = round(d / period)
            if 1 <= k <= 8 and abs(d - k * period) <= 0.02 * k * period + 0.006:
                score += 1.0 / k
        if score > best_score + 1e-9:
            best, best_score = bpm, score
        bpm += 0.1
    if best is None:
        return None
    # the densest pulse explains the most onsets; when only every second onset is accented (8th notes over a 90 BPM beat) the half tempo is the pulse
    if best / 2.0 >= MIN_BPM:
        ratio = _accent_ratio(peaks, 120.0 / best)
        if ratio is not None and ratio >= ACCENT_RATIO:
            best /= 2.0
    est = _explained(iois, 60.0 / best)
    return 60.0 / (sum(est) / len(est)) if est else best


def estimate_tempo(path):
    """Tempo of a WAV file in BPM (60-180, rounded to 0.1), or None when no steady pulse is found."""
    energies, hop_s = _frame_energies(path)
    bpm = _estimate(_onsets(energies, hop_s))
    return None if bpm is None else round(bpm, 1)


# -- the tool -----------------------------------------------------------------------------------------------------------
def _base_url(core):
    url = core.url.rstrip("/")
    return url[:-4] if url.endswith("/mcp") else url


def _download(core, path, dest):
    req = urllib.request.Request(_base_url(core) + path, headers={"Authorization": "Bearer " + core.token} if core.token else {})
    with urllib.request.urlopen(req, timeout=core.timeout) as r, open(dest, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)


def tempo_map(core, params, download=None):
    """analysis.tempo_map {project_id, source: "master"|{track_id}, range?, apply?} -> {bpm, applied}."""
    import os
    import tempfile
    project_id = params.get("project_id")
    if not project_id:
        raise ValueError("missing parameter(s): project_id")
    if not isinstance(project_id, str) or "/" in project_id or "\\" in project_id or ".." in project_id:
        raise ValueError("project_id must not contain path separators")
    source = params.get("source", "master")
    args = {"project_id": project_id, "name": "tempo_probe", "format": {"container": "wav", "bit_depth": 16, "channels": 1}, "wait_s": 60}     # the core caps wait_s at 60
    if source == "master":
        args["scope"] = "master"
    elif isinstance(source, dict) and source.get("track_id"):
        args["scope"] = "tracks"
        args["track_ids"] = [source["track_id"]]
    else:
        raise ValueError('source must be "master" or {track_id}')
    if params.get("range"):
        args["range"] = params["range"]
    render = core.call("render.start", **args)
    for _ in range(10):                     # a long render: keep waiting on the job, 60 s at a time (up to ~11 min in all)
        if render.get("state") not in ("queued", "running") or not render.get("job_id"):
            break
        render = core.call("job.get", job_id=render["job_id"], wait_s=60)
    if render.get("state") != "succeeded":
        raise RuntimeError("the render did not finish: %s" % render.get("state"))
    files = render.get("result", {}).get("file") or (render.get("result", {}).get("files") or [None])[0] or {}
    name = (files.get("path") or "").split("/")[-1]
    if not name:
        raise RuntimeError("the render has no file")
    if "/" in name or "\\" in name or ".." in name:
        raise ValueError("the render name must not contain path separators")
    fetch = download or _download
    with tempfile.TemporaryDirectory() as tmp:
        dest = os.path.join(tmp, name)
        fetch(core, "/api/projects/%s/renders/%s" % (urllib.parse.quote(project_id, safe=""), urllib.parse.quote(name, safe="")), dest)
        bpm = estimate_tempo(dest)
    if bpm is None:
        raise RuntimeError("no steady tempo found in the audio")
    applied = False
    if params.get("apply"):
        core.call("tempo.set", project_id=project_id, points=[{"bpm": bpm}])
        applied = True
    return {"bpm": bpm, "applied": applied}
