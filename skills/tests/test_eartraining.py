# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""The ear-training skill against a fake core: async recipe.run, job.get polling, answers.json apart from the clips."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from rovox_skills import eartraining, server, skills  # noqa: E402
from rovox_skills.core import CoreError  # noqa: E402


class FakeCore:
    url = "http://127.0.0.1:4810/mcp"
    token = ""
    timeout = 5

    def __init__(self, states=("running", "succeeded"), count=3):
        self.calls = []
        self.states = list(states)
        self.count = count

    def call(self, name, /, **args):
        self.calls.append((name, args))
        if name == "recipe.run":
            return {"job_id": "job_1", "state": "queued"}
        if name == "job.get":
            state = self.states.pop(0) if len(self.states) > 1 else self.states[0]
            if state == "succeeded":
                return {"state": state, "result": {
                    "recipe": "eartraining.intervals.v1", "seed": 7, "count": self.count, "tempo": 80, "target_lufs": -16,
                    "clips": [{"name": "clip_%03d.mp3" % (i + 1), "url": "/dl/clip_%03d.mp3?sig=x" % (i + 1), "sha256": "h%d" % i, "variant": i} for i in range(self.count)],
                    "answers": [{"answer": "P5", "tags": ["interval"], "seed": 7, "item_seed": 100 + i} for i in range(self.count)]}}
            if state == "failed":
                return {"state": state, "error": {"code": "QUOTA_EXCEEDED", "message": "too much audio"}}
            return {"state": state}
        return {}


class EarTrainingTests(unittest.TestCase):
    def test_runs_the_recipe_async_and_polls_with_a_capped_wait(self):
        core = FakeCore()
        out = eartraining.generate(core, kind="intervals", count=3, seed=7, items_spec=["P5"], bogus=1)
        name, args = core.calls[0]
        self.assertEqual((name, args["recipe_id"], args["async"]), ("recipe.run", "eartraining.intervals.v1", True))
        self.assertEqual(args["params"], {"count": 3, "seed": 7, "items_spec": ["P5"]})
        polls = [a for n, a in core.calls if n == "job.get"]
        self.assertEqual(len(polls), 2)
        self.assertTrue(all(a["job_id"] == "job_1" and a["wait_s"] <= 60 for a in polls))
        self.assertEqual(len(out["clips"]), 3)
        self.assertEqual(len(out["answers"]), 3)

    def test_out_dir_downloads_clips_and_writes_a_separate_answers_json(self):
        got = []
        with tempfile.TemporaryDirectory() as d:
            out = eartraining.generate(FakeCore(), kind="intervals", out_dir=d, download=lambda url, dest: got.append((url, dest)) or open(dest, "wb").close())
            self.assertEqual(out["files"], ["clip_001.mp3", "clip_002.mp3", "clip_003.mp3"])
            self.assertEqual([os.path.basename(g[1]) for g in got], out["files"])
            with open(os.path.join(d, "answers.json")) as f:
                manifest = json.load(f)
            self.assertEqual(len(manifest["answers"]), 3)
            self.assertEqual(manifest["clips"][0], {"file": "clip_001.mp3", "sha256": "h0"})
            self.assertNotIn("answer", json.dumps(out["files"]))

    def test_a_failed_job_raises_the_core_error_code(self):
        with self.assertRaises(CoreError) as cm:
            eartraining.generate(FakeCore(states=("failed",)), kind="chords")
        self.assertEqual(cm.exception.code, "QUOTA_EXCEEDED")

    def test_unknown_kind_and_timeout(self):
        with self.assertRaises(ValueError):
            eartraining.generate(FakeCore(), kind="rhythm")
        with self.assertRaises(RuntimeError):
            eartraining.generate(FakeCore(states=("running",)), kind="chords", timeout_s=0, sleep=lambda s: None)

    def test_registered_as_skill_and_mcp_tool(self):
        self.assertIn("ear_training", skills.REGISTRY)
        self.assertIn("eartraining.generate", [t["name"] for t in server.TOOLS])
        fake = FakeCore()
        msg = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "eartraining.generate", "arguments": {"kind": "cadences", "count": 3}}}
        r = server.handle(msg, lambda name: fake)
        self.assertTrue(r["result"]["structuredContent"]["ok"])
        self.assertEqual(len(r["result"]["structuredContent"]["data"]["answers"]), 3)
        bad = server.handle(dict(msg, params={"name": "eartraining.generate", "arguments": {"kind": "x"}}), lambda name: fake)
        self.assertEqual(bad["result"]["structuredContent"]["error"]["code"], "SKILL_FAILED")


if __name__ == "__main__":
    unittest.main()
