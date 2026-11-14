# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Ear-training skill: drives the core's eartraining.{intervals,chords,cadences,dictation}.v1 recipes over MCP (recipe.run async, job.get polling),
optionally downloads the clips and writes answers.json next to them, apart from the audio (nothing about an answer is in the clip names or tags)."""
import json
import os
import time
import urllib.parse
import urllib.request

from .core import CoreError
from .skills import skill

KINDS = ("intervals", "chords", "cadences", "dictation")
_PARAM_KEYS = ("count", "items_spec", "range", "tempo", "direction", "bars", "template_id", "seed", "format", "target_lufs", "name", "ttl_s")
_MAX_WAIT_S = 60
_DEFAULT_TIMEOUT_S = 600

PARAMS = {"kind": "intervals|chords|cadences|dictation", "count": "int 1..50 (default 10)", "items_spec": "list of items (see the recipe schema)", "range": "{low, high} MIDI pitches",
          "tempo": "bpm", "direction": "asc|desc|harmonic", "bars": "dictation only, 2..4", "seed": "int or string (same seed, same exercises)", "format": "mp3|wav", "target_lufs": "number (default -16)",
          "template_id": "instrument template (default ear-training)", "out_dir": "directory: download clips as clip_###.* and write answers.json there", "timeout_s": "overall wait for the job (default 600)"}


def _base_url(core):
    url = core.url.rstrip("/")
    return url[:-4] if url.endswith("/mcp") else url


def _fetch(core, url, dest):
    base = _base_url(core)
    full = urllib.parse.urljoin(base + "/", url)
    same_host = urllib.parse.urlsplit(full).netloc == urllib.parse.urlsplit(base).netloc
    headers = {"Authorization": "Bearer " + core.token} if core.token and same_host and not urllib.parse.urlsplit(full).query else {}
    with urllib.request.urlopen(urllib.request.Request(full, headers=headers), timeout=core.timeout) as r, open(dest, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)


def _wait(core, job_id, timeout_s, sleep):
    deadline = time.monotonic() + timeout_s
    while True:
        job = core.call("job.get", job_id=job_id, wait_s=_MAX_WAIT_S)
        state = job.get("state")
        if state == "succeeded":
            return job.get("result") or {}
        if state in ("failed", "cancelled", "canceled"):
            err = job.get("error") or {}
            raise CoreError("job.get", {"code": err.get("code", "JOB_" + str(state).upper()), "message": err.get("message", "the ear-training job " + str(state)), "details": err.get("details")})
        if time.monotonic() >= deadline:
            raise RuntimeError("timed out waiting for job %s (state %s)" % (job_id, state))
        sleep(0)


def generate(core, kind=None, out_dir=None, timeout_s=_DEFAULT_TIMEOUT_S, download=None, sleep=time.sleep, **params):
    """Runs eartraining.<kind>.v1 as an async recipe job, waits for it, and returns {recipe, seed, clips, answers, ...}. With out_dir the clips
    are downloaded and answers.json (answers, seed, per-clip file and sha256) is written there as a separate file."""
    if kind not in KINDS:
        raise ValueError("kind must be one of: " + ", ".join(KINDS))
    recipe_params = {k: params[k] for k in _PARAM_KEYS if params.get(k) is not None}
    started = core.call("recipe.run", **{"recipe_id": "eartraining.%s.v1" % kind, "params": recipe_params, "async": True})
    result = _wait(core, started["job_id"], float(timeout_s), sleep)
    out = {k: result[k] for k in ("recipe", "seed", "count", "tempo", "target_lufs", "clips", "answers") if k in result}
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        fetch = download or (lambda url, dest: _fetch(core, url, dest))
        files = []
        for i, clip in enumerate(out.get("clips", [])):
            url = clip.get("url")
            if not url:
                raise ValueError("clip %d has no url" % i)
            name = os.path.basename(clip.get("name") or urllib.parse.urlsplit(url).path)
            fetch(url, os.path.join(out_dir, name))
            files.append({"file": name, "sha256": clip.get("sha256")})
        manifest = {"recipe": out.get("recipe"), "seed": out.get("seed"), "clips": files, "answers": out.get("answers", [])}
        path = os.path.join(out_dir, "answers.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
        out["out_dir"] = out_dir
        out["answers_path"] = path
        out["files"] = [f["file"] for f in files]
    return out


@skill("ear_training", "Generates seeded ear-training clips (intervals, chords, cadences, dictation) with the core's eartraining recipes; with out_dir downloads them and writes answers.json beside them.", PARAMS)
def ear_training(core, **params):
    return generate(core, **params)


EARTRAINING_TOOLS = [
    {"name": "eartraining.generate", "description": "Runs the core's eartraining.<kind>.v1 recipe as a job (async, polled with job.get) and returns the clips (signed urls, sha256, loudness) and the answers apart from them. "
     "With out_dir the clips are downloaded as clip_###.* and answers.json is written beside them. The same seed gives the same exercises.",
     "inputSchema": {"type": "object", "properties": {"kind": {"type": "string", "enum": list(KINDS)}, "count": {"type": "integer"}, "items_spec": {"type": "array", "items": {}}, "range": {"type": "object"},
                                                        "tempo": {"type": "number"}, "direction": {"type": "string", "enum": ["asc", "desc", "harmonic"]}, "bars": {"type": "integer"}, "seed": {"type": ["integer", "string"]},
                                                        "format": {"type": "string", "enum": ["mp3", "wav"]}, "target_lufs": {"type": "number"}, "template_id": {"type": "string"},
                                                        "out_dir": {"type": "string"}, "timeout_s": {"type": "number"}}, "required": ["kind"]}},
]


def _text(ok, data=None, error=None):
    envelope = {"ok": ok}
    envelope["data" if ok else "error"] = data if ok else error
    return {"content": [{"type": "text", "text": json.dumps(envelope)}], "structuredContent": envelope, "isError": not ok}


def wrap_handler(inner):
    """Wraps the server's _handle(message, core_factory): eartraining.generate is answered here."""
    def handle(message, core_factory):
        if isinstance(message, dict) and message.get("method") == "tools/call" and message.get("id") is not None:
            p = message.get("params", {})
            if p.get("name") == "eartraining.generate":
                try:
                    out = _text(True, generate(core_factory("skill:eartraining"), **(p.get("arguments") or {})))
                except CoreError as e:
                    out = _text(False, error={"code": e.code, "message": str(e), "details": e.details})
                except (KeyError, ValueError, TypeError, RuntimeError, OSError) as e:
                    out = _text(False, error={"code": "SKILL_FAILED", "message": "%s: %s" % (type(e).__name__, e)})
                return {"jsonrpc": "2.0", "id": message["id"], "result": out}
        return inner(message, core_factory)
    return handle
