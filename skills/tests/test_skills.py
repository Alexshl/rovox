# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Contract tests against a fake core: the skills only call core tools, deterministically, and no secret is ever in a response."""
import io
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from rovox_skills import server, skills  # noqa: E402
from rovox_skills.core import CoreError  # noqa: E402


class FakeCore:
    """Records tool calls and answers like the real core would."""

    def __init__(self):
        self.calls = []
        self.tracks = []
        self.instruments = [{"plugin_uid": "lv2:urn:zyn", "name": "ZynAddSubFX"}, {"plugin_uid": "lv2:urn:drums", "name": "Test Drum Kit"}]

    def call(self, tool, **args):
        self.calls.append((tool, args))
        if tool == "track.list":
            return {"tracks": self.tracks}
        if tool == "plugin.catalog":
            q = args["query"].lower()
            return {"plugins": [p for p in self.instruments if q in p["name"].lower() or q in p["plugin_uid"]][:1]}
        if tool == "track.create":
            self.tracks.append({"track_id": "trk_%d" % (len(self.tracks) + 1), "name": args["name"]})
            return {"track_id": self.tracks[-1]["track_id"]}
        if tool == "item.create_midi":
            return {"item_id": "itm_%d" % len(self.calls)}
        if tool == "routing.send_create":
            return {"send_id": "snd_%d" % len(self.calls)}
        if tool == "render.start":
            return {"state": "succeeded", "job_id": "job_1", "result": {"file": {"path": "renders/mix.mp3"}, "stats": {"integrated_lufs": -14.0}}}
        if tool == "delivery.send":
            return {"delivery_id": "dlv_1", "status": "sent", "receipt": {"message_id": 1}}
        if tool == "render.preview":
            return {"result": {"loudness": {"integrated_lufs": -20.0, "true_peak_dbtp": -3.0}}}
        return {}


class PatternTests(unittest.TestCase):
    def test_same_seed_gives_identical_midi_and_other_seeds_differ(self):
        a, b, c = skills.drum_pattern(4, seed=7), skills.drum_pattern(4, seed=7), skills.drum_pattern(4, seed=8)
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)
        self.assertEqual(skills.bass_pattern(36, 4, seed=3), skills.bass_pattern(36, 4, seed=3))

    def test_drum_pattern_has_a_backbeat_and_stays_inside_the_bars(self):
        notes = skills.drum_pattern(2, seed=1)
        kicks = [n["start_qn"] for n in notes if n["pitch"] == skills.KICK]
        snares = [n["start_qn"] for n in notes if n["pitch"] == skills.SNARE]
        self.assertIn(0, kicks)
        self.assertEqual(snares, [1, 3, 5, 7])
        self.assertTrue(all(0 <= n["start_qn"] < 8 and 1 <= n["velocity"] <= 127 for n in notes))

    def test_bass_pattern_is_in_the_scale(self):
        notes = skills.bass_pattern(36, 2, seed=2)
        self.assertTrue(all((n["pitch"] - 36) in (0, 3, 5, 7, 10) for n in notes))
        self.assertEqual(notes[0]["pitch"], 36)


class SkillTests(unittest.TestCase):
    def test_rhythm_section_creates_tracks_loads_instruments_and_writes_midi(self):
        core = FakeCore()
        report = skills.run_skill(core, "rhythm_section", {"project_id": "p_1", "bars": 2, "seed": 5})
        tools = [c[0] for c in core.calls]
        self.assertEqual(tools.count("track.create"), 2)
        self.assertEqual(tools.count("fx.add"), 2)
        self.assertEqual(tools.count("item.create_midi"), 2)
        self.assertEqual(report["tracks"]["Drums"]["instrument"], "Test Drum Kit")
        midi = [c[1] for c in core.calls if c[0] == "item.create_midi"]
        self.assertEqual(midi[0]["notes"], skills.drum_pattern(2, 5))
        self.assertTrue(all(c[1].get("project_id") == "p_1" for c in core.calls if c[0] != "plugin.catalog"))

    def test_rhythm_section_reuses_tracks_and_reports_a_missing_instrument(self):
        core = FakeCore()
        core.tracks = [{"track_id": "trk_9", "name": "drums"}, {"track_id": "trk_8", "name": "Bass"}]
        skills.run_skill(core, "rhythm_section", {"project_id": "p_1"})
        self.assertNotIn("track.create", [c[0] for c in core.calls])
        empty = FakeCore()
        empty.instruments = []
        with self.assertRaises(ValueError) as cm:
            skills.run_skill(empty, "rhythm_section", {"project_id": "p_1"})
        self.assertIn("plugin.catalog", str(cm.exception))

    def test_render_and_deliver_chains_the_render_job_into_the_delivery(self):
        core = FakeCore()
        out = skills.run_skill(core, "render_and_deliver", {"project_id": "p_1", "adapter": "telegram.me", "target_lufs": -14, "caption": "v3"})
        render = [c for c in core.calls if c[0] == "render.start"][0][1]
        self.assertEqual(render["target_lufs"], -14.0)
        self.assertTrue(render["check"])
        delivery = [c for c in core.calls if c[0] == "delivery.send"][0][1]
        self.assertEqual(delivery["source"], {"job_id": "job_1"})
        self.assertEqual(out["delivery"]["status"], "sent")
        with self.assertRaises(ValueError):
            skills.run_skill(core, "render_and_deliver", {"project_id": "p_1"})

    def test_mastering_check_changes_nothing(self):
        core = FakeCore()
        out = skills.run_skill(core, "mastering_check", {"project_id": "p_1", "target_lufs": -14})
        self.assertEqual(out["verdict"], "too quiet")
        self.assertEqual(out["gain_needed_db"], 6.0)
        self.assertEqual([c[0] for c in core.calls], ["render.preview"])

    def test_unknown_skill(self):
        with self.assertRaises(KeyError):
            skills.run_skill(FakeCore(), "nope", {})


class ServerTests(unittest.TestCase):
    def test_mcp_server_lists_and_runs_skills_without_leaking_keys(self):
        os.environ["ANTHROPIC_API_KEY"] = "sk-ant-should-never-appear"
        out = io.StringIO()
        lines = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "skill.list", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "skill.run", "arguments": {"name": "add_reverb_bus", "params": {"project_id": "p_1", "track_ids": ["trk_1"]}}}},
            {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "skill.run", "arguments": {"name": "nope"}}},
        ]
        fake = FakeCore()
        for m in lines:
            r = server.handle(m, lambda name: fake)
            out.write(json.dumps(r) + "\n")
        replies = [json.loads(x) for x in out.getvalue().splitlines()]
        self.assertEqual({t["name"] for t in replies[1]["result"]["tools"]}, {"skill.list", "skill.run"})
        self.assertIn("rhythm_section", json.dumps(replies[2]))
        self.assertTrue(replies[3]["result"]["structuredContent"]["ok"])
        self.assertEqual(replies[4]["result"]["structuredContent"]["error"]["code"], "SKILL_FAILED")
        self.assertNotIn("sk-ant-should-never-appear", out.getvalue())

    def test_core_errors_become_tool_errors(self):
        class Failing(FakeCore):
            def call(self, tool, **args):
                raise CoreError(tool, {"code": "PRECONDITION_FAILED", "message": "nope"})
        r = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "skill.run", "arguments": {"name": "add_reverb_bus", "params": {"project_id": "p"}}}}, lambda n: Failing())
        self.assertEqual(r["result"]["structuredContent"]["error"]["code"], "PRECONDITION_FAILED")


if __name__ == "__main__":
    unittest.main()
