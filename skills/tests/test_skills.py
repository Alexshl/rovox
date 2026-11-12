# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Contract tests against a fake core: the skills only call core tools, deterministically, and no secret is ever in a response."""
import io
import json
import math
import os
import sys
import struct
import tempfile
import threading
import unittest
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from rovox_skills import analysis, server, skills  # noqa: E402
from rovox_skills.core import Core, CoreError  # noqa: E402


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
        self.assertTrue({"skill.list", "skill.run", "analysis.tempo_map"} <= {t["name"] for t in replies[1]["result"]["tools"]})
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


def click_wav(path, bpm, seconds=10, rate=22050, lead_s=0.0, per_beat=1, offbeat_gain=1.0):
    """Clicks every beat (or `per_beat` times per beat, the ones between the beats scaled by offbeat_gain) after `lead_s` of digital silence."""
    n = int(rate * seconds)
    period = 60.0 / bpm * rate / per_beat
    samples = [0] * n
    k = 0
    while int(lead_s * rate + k * period) < n:
        start = int(lead_s * rate + k * period)
        gain = 1.0 if k % per_beat == 0 else offbeat_gain
        for i in range(min(200, n - start)):
            samples[start + i] = int(20000 * gain * math.exp(-i / 30.0) * (1 if i % 2 == 0 else -1))
        k += 1
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % n, *samples))


class _FakeCoreHttp:
    """A minimal Streamable HTTP core: sessions, 404 for an unknown session, DELETE."""

    def __init__(self):
        self.sessions = set()
        self.initializes = 0
        self.deleted = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _reply(self, code, body=b"", headers=None):
                self.send_response(code)
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_DELETE(self):
                outer.deleted.append(self.headers.get("Mcp-Session-Id"))
                outer.sessions.discard(self.headers.get("Mcp-Session-Id"))
                self._reply(200)

            def do_POST(self):
                message = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if message.get("method") == "initialize":
                    outer.initializes += 1
                    sid = "s%d" % outer.initializes
                    outer.sessions.add(sid)
                    body = json.dumps({"jsonrpc": "2.0", "id": message["id"], "result": {}}).encode()
                    return self._reply(200, body, {"Content-Type": "application/json", "Mcp-Session-Id": sid})
                if self.headers.get("Mcp-Session-Id") not in outer.sessions:
                    return self._reply(404, b'{"error":"unknown session"}')
                if "id" not in message:
                    return self._reply(202)
                envelope = {"ok": True, "data": {"echo": message["params"]["arguments"]}}
                body = json.dumps({"jsonrpc": "2.0", "id": message["id"], "result": {"structuredContent": envelope}}).encode()
                self._reply(200, body, {"Content-Type": "application/json"})

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d/mcp" % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class SessionRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.core_http = _FakeCoreHttp()
        self.addCleanup(self.core_http.stop)

    def test_a_404_starts_a_new_session_and_repeats_the_call(self):
        core = Core(self.core_http.url, "", name="skill:test")
        self.assertEqual(core.call("x.y", a=1), {"echo": {"a": 1}})
        self.assertEqual(self.core_http.initializes, 1)
        self.core_http.sessions.clear()         # the core restarted
        self.assertEqual(core.call("x.y", a=2), {"echo": {"a": 2}})
        self.assertEqual(self.core_http.initializes, 2)

    def test_close_deletes_the_session(self):
        core = Core(self.core_http.url, "", name="skill:test")
        core.call("x.y")
        core.close()
        self.assertEqual(self.core_http.deleted, ["s1"])

    def test_one_session_serves_two_skill_runs_and_is_deleted_at_exit(self):
        run = {"jsonrpc": "2.0", "method": "tools/call", "params": {"name": "analysis.tempo_map", "arguments": {"project_id": "p"}}}
        lines = [json.dumps(dict(run, id=1)), json.dumps(dict(run, id=2))]
        created = []
        original = server.Core

        def factory(url, token, name=None):
            created.append(name)
            return original(url, token, name=name)
        original_tempo = analysis.tempo_map
        server.Core = factory
        analysis.tempo_map = lambda core, args: core.call("x.y", n=1)
        try:
            server.serve(self.core_http.url, None, stdin=io.StringIO("\n".join(lines) + "\n"), stdout=io.StringIO())
        finally:
            server.Core = original
            analysis.tempo_map = original_tempo
        self.assertEqual(len(created), 1)
        self.assertEqual(self.core_http.initializes, 1)
        self.assertEqual(self.core_http.deleted, ["s1"])


class SecretTests(unittest.TestCase):
    def setUp(self):
        self._env = dict(os.environ)
        os.environ["ROVOX_TOKEN"] = "tok-planted-12345"

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)

    def call(self, factory, token=None):
        r = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "skill.run", "arguments": {"name": "add_reverb_bus", "params": {"project_id": "p"}}}}, factory, token)
        return json.dumps(r)

    def test_token_in_a_core_error_body_is_redacted(self):
        class Failing(FakeCore):
            def call(self, tool, **args):
                raise CoreError(tool, {"code": "X", "message": "bad Bearer tok-planted-12345", "details": {"echo": "tok-planted-12345", "other": "bearer-from-arg-777"}})
        out = self.call(lambda n: Failing(), "bearer-from-arg-777")
        self.assertNotIn("tok-planted-12345", out)
        self.assertNotIn("bearer-from-arg-777", out)
        self.assertIn("[redacted]", out)

    def test_token_in_a_protocol_error_is_redacted(self):
        class Failing(FakeCore):
            def call(self, tool, **args):
                raise RuntimeError("the core answered HTTP 500: Authorization: Bearer tok-planted-12345")
        self.assertNotIn("tok-planted-12345", self.call(lambda n: Failing()))
        os.environ["MY_API_KEY"] = "key-value-abcdef"
        r = server.handle({"jsonrpc": "2.0", "id": 2, "method": "nope key-value-abcdef"}, lambda n: None)
        self.assertNotIn("key-value-abcdef", json.dumps(r))


class ServerRobustnessTests(unittest.TestCase):
    def setUp(self):
        self._env = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self._env)

    def test_short_secrets_are_kept_and_long_ones_with_more_names_are_redacted(self):
        os.environ["MY_PASSWORD"] = "hunter2hunter2hunter2"
        os.environ["DB_CREDENTIAL"] = "cred-value-1234567"
        os.environ["SSH_PASS"] = "pass-value-1234567"
        os.environ["SHORT_TOKEN"] = "abc123"
        out = json.dumps(server.redact({"a": "hunter2hunter2hunter2 cred-value-1234567 pass-value-1234567 abc123"}))
        self.assertNotIn("hunter2hunter2hunter2", out)
        self.assertNotIn("cred-value-1234567", out)
        self.assertNotIn("pass-value-1234567", out)
        self.assertIn("abc123", out)        # below the minimum length: not treated as a secret

    def test_an_uncaught_exception_becomes_a_redacted_error_result(self):
        os.environ["MY_API_KEY"] = "key-value-abcdef-123"

        class Boom(FakeCore):
            def call(self, tool, **args):
                raise wave.Error("bad key-value-abcdef-123")
        message = {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "skill.run", "arguments": {"name": "add_reverb_bus", "params": {"project_id": "p"}}}}
        r = server.handle(message, lambda n: Boom())
        self.assertEqual(r["id"], 7)
        self.assertFalse(r["result"]["structuredContent"]["ok"])
        self.assertEqual(r["result"]["structuredContent"]["error"]["code"], "INTERNAL_ERROR")
        self.assertNotIn("key-value-abcdef-123", json.dumps(r))

    def test_the_stdio_loop_survives_a_failing_request(self):
        class Boom(FakeCore):
            def call(self, tool, **args):
                raise EOFError("truncated")
        lines = [json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "skill.run", "arguments": {"name": "add_reverb_bus", "params": {"project_id": "p"}}}}),
                 "not json",
                 json.dumps({"jsonrpc": "2.0", "id": 2, "method": "ping"})]
        out = io.StringIO()
        original = server.Core
        server.Core = lambda url, token, name=None: Boom()
        try:
            server.serve(None, None, stdin=io.StringIO("\n".join(lines) + "\n"), stdout=out)
        finally:
            server.Core = original
        replies = [json.loads(x) for x in out.getvalue().splitlines()]
        self.assertEqual(len(replies), 3)
        self.assertEqual(replies[2]["id"], 2)


class TempoTests(unittest.TestCase):
    def test_click_track_at_120_bpm(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "click.wav")
            click_wav(path, 120)
            self.assertAlmostEqual(analysis.estimate_tempo(path), 120, delta=0.5)
            click_wav(path, 93)
            self.assertAlmostEqual(analysis.estimate_tempo(path), 93, delta=0.5)

    def test_click_track_after_a_silent_lead_in(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "click.wav")
            click_wav(path, 100, seconds=12, lead_s=2.0)
            self.assertAlmostEqual(analysis.estimate_tempo(path), 100, delta=0.5)

    def test_accented_eighth_notes_at_90_bpm_are_not_doubled(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "eighths.wav")
            click_wav(path, 90, seconds=16, per_beat=2, offbeat_gain=0.4)
            self.assertAlmostEqual(analysis.estimate_tempo(path), 90, delta=0.5)

    def test_tempo_map_quotes_the_download_path_and_rejects_separators(self):
        class Core(FakeCore):
            url, token, timeout = "http://x/mcp", "t", 5

            def call(self, tool, **args):
                if tool == "render.start":
                    return {"state": "succeeded", "result": {"file": {"path": "renders/my tempo.wav"}}}
                return super().call(tool, **args)
        fetched = []

        def download(core, path, dest):
            fetched.append(path)
            click_wav(dest, 120)
        analysis.tempo_map(Core(), {"project_id": "p 1?x"}, download)
        self.assertEqual(fetched, ["/api/projects/p%201%3Fx/renders/my%20tempo.wav"])
        for bad in ("../p", "a/b", "a..b"):
            with self.assertRaises(ValueError):
                analysis.tempo_map(Core(), {"project_id": bad}, download)

    def test_tempo_map_renders_downloads_and_applies(self):
        class Core(FakeCore):
            url, token, timeout = "http://x/mcp", "t", 5

            def call(self, tool, **args):
                if tool == "render.start":
                    self.calls.append((tool, args))
                    return {"state": "succeeded", "result": {"file": {"path": "renders/tempo_probe.wav"}}}
                return super().call(tool, **args)
        fetched = []

        def download(core, path, dest):
            fetched.append(path)
            click_wav(dest, 120)
        core = Core()
        out = analysis.tempo_map(core, {"project_id": "p_1", "source": {"track_id": "trk_1"}, "apply": True}, download)
        self.assertAlmostEqual(out["bpm"], 120, delta=0.5)
        self.assertTrue(out["applied"])
        self.assertEqual(fetched, ["/api/projects/p_1/renders/tempo_probe.wav"])
        tools = [c[0] for c in core.calls]
        self.assertEqual(tools, ["render.start", "tempo.set"])
        self.assertEqual(core.calls[0][1]["track_ids"], ["trk_1"])
        core2 = Core()
        self.assertFalse(analysis.tempo_map(core2, {"project_id": "p_1"}, download)["applied"])
        self.assertNotIn("tempo.set", [c[0] for c in core2.calls])


if __name__ == "__main__":
    unittest.main()
