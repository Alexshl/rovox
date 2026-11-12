# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Contract tests of the notation tools: MusicXML import, transposition, engraving through fake external programs, SKILL_UNAVAILABLE paths, clean errors."""
import io
import json
import os
import stat
import struct
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from rovox_skills import server  # noqa: E402
from rovox_skills.notation import abc, musicxml, transpose  # noqa: E402
from rovox_skills.notation.errors import NotationError  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
WALTZ = os.path.join(FIXTURES, "waltz_g.musicxml")


class FakeCore:
    def __init__(self):
        self.calls = []

    def call(self, tool, **args):
        self.calls.append((tool, args))
        return {"item_id": "itm_1"}


def call(name, args, core=None):
    """One tools/call through the real request handler; returns the envelope {ok, data|error}."""
    core = core or FakeCore()
    r = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}}, lambda n: core)
    return r["result"]["structuredContent"]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = os.path.realpath(self.tmp.name)
        env = mock.patch.dict(os.environ, {"ROVOX_SKILLS_DIRS": os.pathsep.join([self.dir, FIXTURES])})
        env.start()
        self.addCleanup(env.stop)

    def write(self, name, content, mode="w"):
        path = os.path.join(self.dir, name)
        with open(path, mode) as f:
            f.write(content)
        return path


class ImportTests(Base):
    def test_fixture_with_repeats_expanded(self):
        r = call("notation.import", {"file": WALTZ})["data"]
        self.assertEqual((r["key"], r["mode"], r["key_source"], r["key_confidence"]), ("G", "major", "signature", 1.0))
        self.assertEqual(r["time_signature"], {"beats": 3, "beat_type": 4})
        self.assertEqual(r["tempo"], 96.0)
        self.assertEqual(r["pickup_qn"], 1.0)
        self.assertEqual(r["bars"], 22)         # 16 written measures minus the pickup, plus the repeated 7 bars
        self.assertEqual(r["length_qn"], 1.0 + 22 * 3.0)
        self.assertEqual(r["warnings"], [])
        notes = r["melody"]["notes"]
        self.assertEqual(notes[0], {"pitch": 62, "start_qn": 0.0, "length_qn": 1.0})              # the pickup D4
        self.assertEqual(notes[1], {"pitch": 67, "start_qn": 1.0, "length_qn": 2.0})
        self.assertIn({"pitch": 71, "start_qn": 4.0, "length_qn": 4.0}, notes)                    # B4 tied across the barline: 3 + 1 quarter notes
        triplet = [n for n in notes if n["start_qn"] == 10.0 or abs(n["start_qn"] - 10.333333) < 1e-6]
        self.assertEqual([n["pitch"] for n in triplet], [69, 71])
        self.assertTrue(all(abs(n["length_qn"] - 1 / 3) < 1e-5 for n in triplet))
        chord = sorted(n["pitch"] for n in notes if n["start_qn"] == 13.0)
        self.assertEqual(chord, [67, 71, 74])
        self.assertEqual([c["symbol"] for c in r["chords"]][:6], ["G", "Cmaj7", "D7", "Em", "G/B", "D7"])
        self.assertEqual(r["chords"][0], {"bar": 1, "beat": 1.0, "symbol": "G"})

    def test_first_and_second_endings(self):
        expanded = musicxml.import_musicxml(WALTZ)["melody"]["notes"]
        plain_info = musicxml.import_musicxml(WALTZ, expand_repeats=False)
        plain = plain_info["melody"]["notes"]
        self.assertEqual(plain_info["bars"], 16)
        self.assertEqual(plain_info["length_qn"], 1.0 + 16 * 3.0)

        def count(notes, pitch, length):
            return sum(1 for n in notes if n["pitch"] == pitch and n["length_qn"] == length)
        self.assertEqual((count(expanded, 71, 4.0), count(plain, 71, 4.0)), (2, 1))        # the tied B4 of the repeated section
        self.assertEqual((count(expanded, 66, 3.0), count(plain, 66, 3.0)), (1, 1))        # F#4 only in ending 1, played on the first pass only
        self.assertEqual((count(expanded, 64, 3.0), count(plain, 64, 3.0)), (1, 1))        # E4 only in ending 2
        first_ending = [n["start_qn"] for n in expanded if n["pitch"] == 66 and n["length_qn"] == 3.0][0]
        second_ending = [n["start_qn"] for n in expanded if n["pitch"] == 64 and n["length_qn"] == 3.0][0]
        self.assertLess(first_ending, second_ending)
        self.assertEqual(second_ending - first_ending, 7 * 3.0)         # pass 2 replays 6 bars of the body, then the second ending

    def test_notes_are_exactly_the_midi_write_format(self):
        for n in call("notation.import", {"file": WALTZ})["data"]["melody"]["notes"]:
            self.assertEqual(set(n), {"pitch", "start_qn", "length_qn"})
            self.assertIsInstance(n["pitch"], int)
            self.assertTrue(0 <= n["pitch"] <= 127 and n["start_qn"] >= 0 and n["length_qn"] > 0)

    def test_mxl_gives_the_same_result(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("META-INF/container.xml", '<?xml version="1.0"?><container><rootfiles><rootfile full-path="score/waltz.xml" media-type="application/vnd.recordare.musicxml+xml"/></rootfiles></container>')
            with open(WALTZ, "rb") as f:
                z.writestr("score/waltz.xml", f.read())
        path = self.write("waltz.mxl", buf.getvalue(), "wb")
        a = call("notation.import", {"file": path})["data"]
        b = call("notation.import", {"file": WALTZ})["data"]
        self.assertEqual(a["melody"], b["melody"])
        self.assertEqual((a["key"], a["tempo"], a["chords"]), (b["key"], b["tempo"], b["chords"]))

    def test_key_estimation_without_a_key_signature(self):
        with open(WALTZ) as f:
            text = f.read().replace("<key><fifths>1</fifths><mode>major</mode></key>", "")
        r = call("notation.import", {"file": self.write("nokey.musicxml", text)})["data"]
        self.assertEqual((r["key"], r["mode"], r["key_source"]), ("G", "major", "estimated"))
        self.assertTrue(0.5 < r["key_confidence"] <= 1.0)
        self.assertTrue(any("estimated" in w for w in r["warnings"]))

    def test_a_minor_estimate(self):
        steps = "AEAEBCAEAEDCBAEAEEAA"      # heavy on A, E, C: A minor
        body = "".join('<note><pitch><step>%s</step><octave>4</octave></pitch><duration>1</duration></note>' % s for s in steps)
        xml = ('<score-partwise><part-list><score-part id="P1"/></part-list><part id="P1"><measure number="1"><attributes><divisions>1</divisions></attributes>%s</measure></part></score-partwise>' % body)
        r = musicxml.parse_score(xml.encode())
        self.assertEqual((r["key"], r["mode"]), ("A", "minor"))

    def test_transposing_instrument_is_returned_at_concert_pitch(self):
        xml = ('<score-partwise><part-list><score-part id="P1"><part-name>Clarinet</part-name></score-part></part-list><part id="P1"><measure number="1"><attributes><divisions>1</divisions>'
               '<key><fifths>3</fifths></key><time><beats>4</beats><beat-type>4</beat-type></time><transpose><diatonic>-1</diatonic><chromatic>-2</chromatic></transpose></attributes>'
               '<note><pitch><step>C</step><octave>5</octave></pitch><duration>4</duration></note></measure></part></score-partwise>')
        concert = musicxml.parse_score(xml.encode())
        self.assertEqual(concert["melody"]["notes"][0]["pitch"], 70)
        self.assertEqual((concert["key"], concert["key_fifths"]), ("G", 1))
        written = musicxml.parse_score(xml.encode(), concert=False)
        self.assertEqual((written["melody"]["notes"][0]["pitch"], written["key"]), (72, "A"))

    def test_parts_and_part_selection(self):
        xml = ('<score-partwise><part-list><score-part id="P1"><part-name>Flute</part-name></score-part><score-part id="P2"><part-name>Bass</part-name></score-part></part-list>'
               '<part id="P1"><measure number="1"><attributes><divisions>1</divisions></attributes><note><pitch><step>C</step><octave>5</octave></pitch><duration>4</duration></note></measure></part>'
               '<part id="P2"><measure number="1"><attributes><divisions>1</divisions></attributes><note><pitch><step>C</step><octave>2</octave></pitch><duration>4</duration></note></measure></part></score-partwise>')
        self.assertEqual(musicxml.parse_score(xml.encode(), part="Bass")["melody"]["notes"][0]["pitch"], 36)
        self.assertEqual(musicxml.parse_score(xml.encode(), part="2")["part"], "P2")
        self.assertEqual(musicxml.parse_score(xml.encode())["part"], "P1")
        self.assertEqual([p["name"] for p in musicxml.parse_score(xml.encode())["parts"]], ["Flute", "Bass"])
        self.assertEqual(call("notation.import", {"file": self.write("two.xml", xml), "part": "nope"})["error"]["code"], "INVALID_PARAMS")

    def test_write_to_creates_a_midi_item_through_the_core(self):
        core = FakeCore()
        r = call("notation.import", {"file": WALTZ, "write_to": {"project_id": "prj_1", "track_id": "trk_1"}}, core)["data"]
        self.assertEqual(r["written"], {"item_id": "itm_1", "notes": len(r["melody"]["notes"])})
        tool, args = core.calls[0]
        self.assertEqual(tool, "item.create_midi")
        self.assertEqual((args["project_id"], args["track_id"], args["notes"]), ("prj_1", "trk_1", r["melody"]["notes"]))


class ErrorTests(Base):
    def err(self, name, args):
        env = call(name, args)
        self.assertFalse(env["ok"], env)
        return env["error"]

    def test_malformed_xml_is_a_clean_error_with_a_line(self):
        e = self.err("notation.import", {"file": self.write("bad.xml", "<score-partwise>\n<part>\n<measure></part>")})
        self.assertEqual(e["code"], "INVALID_PARAMS")
        self.assertIn("malformed XML", e["message"])
        self.assertEqual(e["details"]["line"], 3)

    def test_wrong_root_timewise_empty_and_binary(self):
        self.assertEqual(self.err("notation.import", {"file": self.write("a.xml", "<html/>")})["details"]["element"], "html")
        self.assertIn("timewise", self.err("notation.import", {"file": self.write("b.xml", "<score-timewise/>")})["message"])
        self.assertEqual(self.err("notation.import", {"file": self.write("c.xml", "")})["code"], "INVALID_PARAMS")
        self.assertEqual(self.err("notation.import", {"file": self.write("d.xml", b"\x00\xff\xfe\x01", "wb")})["code"], "INVALID_PARAMS")
        self.assertEqual(self.err("notation.import", {"file": self.write("e.mxl", b"PK\x03\x04garbage", "wb")})["code"], "INVALID_PARAMS")

    def test_missing_duration_names_the_element_and_measure(self):
        xml = '<score-partwise><part-list/><part id="P1"><measure number="7"><note><pitch><step>C</step><octave>4</octave></pitch></note></measure></part></score-partwise>'
        e = self.err("notation.import", {"file": self.write("f.xml", xml)})
        self.assertEqual((e["code"], e["details"]["element"], e["details"]["measure"]), ("INVALID_PARAMS", "duration", "7"))

    def test_entities_are_refused(self):
        xml = '<?xml version="1.0"?><!DOCTYPE x [<!ENTITY a "aaaa">]><score-partwise><part id="P1"/></score-partwise>'
        self.assertIn("entity", self.err("notation.import", {"file": self.write("g.xml", xml)})["message"])

    def test_missing_file_and_file_outside_the_allowed_directories(self):
        self.assertEqual(self.err("notation.import", {"file": os.path.join(self.dir, "nope.xml")})["code"], "INVALID_PARAMS")
        self.assertIn("outside", self.err("notation.import", {"file": "/etc/hosts"})["message"])
        self.assertEqual(self.err("notation.import", {})["code"], "INVALID_PARAMS")
        self.assertEqual(self.err("notation.transpose", {"file": WALTZ})["code"], "INVALID_PARAMS")      # to is required
        self.assertEqual(self.err("notation.transpose", {"file": WALTZ, "to": "Kazoo"})["code"], "INVALID_PARAMS")

    def test_notes_out_of_range_after_transposing(self):
        self.assertEqual(self.err("notation.transpose", {"notes": [{"pitch": 126, "start_qn": 0, "length_qn": 1}], "to": 5, "key_fifths": 0})["code"], "INVALID_PARAMS")


class TransposeTests(Base):
    def test_bb_shifts_two_semitones_and_the_key(self):
        r = call("notation.transpose", {"file": WALTZ, "to": "Bb"})["data"]
        base = musicxml.import_musicxml(WALTZ)
        self.assertEqual([n["pitch"] for n in r["melody"]["notes"]], [n["pitch"] + 2 for n in base["melody"]["notes"]])
        self.assertEqual([(n["start_qn"], n["length_qn"]) for n in r["melody"]["notes"]], [(n["start_qn"], n["length_qn"]) for n in base["melody"]["notes"]])
        self.assertEqual((r["key"], r["key_fifths"], r["mode"], r["semitones"]), ("A", 3, "major", 2))
        self.assertEqual([c["symbol"] for c in r["chords"]][:5], ["A", "Dmaj7", "E7", "F#m", "A/C#"])

    def test_eb_alto_sax_and_others(self):
        r = call("notation.transpose", {"file": WALTZ, "to": "Eb"})["data"]
        self.assertEqual((r["key"], r["semitones"]), ("E", 9))
        self.assertEqual(r["melody"]["notes"][0]["pitch"], 62 + 9)
        self.assertEqual(r["chords"][0]["symbol"], "E")
        self.assertEqual(transpose.resolve_target("F")[0], 7)
        self.assertEqual(transpose.resolve_target("C")[0], 0)
        self.assertEqual(transpose.resolve_target("alto_sax")[0], 9)
        self.assertEqual(transpose.resolve_target("-3")[0], -3)
        self.assertEqual(transpose.resolve_target(3, octave=-1), (-9, "+3 semitones"))
        self.assertEqual(transpose.resolve_target("piccolo")[0], -12)

    def test_flat_keys_are_chosen_with_fewer_accidentals(self):
        self.assertEqual(musicxml.shift_fifths(1, 2), 3)        # G -> A
        self.assertEqual(musicxml.shift_fifths(0, 3), -3)       # C -> Eb, not D#
        self.assertEqual(musicxml.shift_fifths(0, 1), -5)       # C -> Db (5 flats beats C# with 7 sharps)
        self.assertEqual(musicxml.shift_fifths(-2, 2), 0)       # Bb -> C
        self.assertEqual(transpose.transpose_chord("Am7/G", 3, -3), "Cm7/Bb")
        self.assertEqual(transpose.transpose_chord("F#m", -2, 0), "Em")

    def test_octave_and_range_warning(self):
        r = call("notation.transpose", {"notes": [{"pitch": 100, "start_qn": 0, "length_qn": 1}], "to": "C", "octave": 1, "key_fifths": 0})["data"]
        self.assertEqual(r["melody"]["notes"][0]["pitch"], 112)
        self.assertTrue(any("range" in w for w in r["warnings"]))

    def test_written_musicxml_reads_back_identically(self):
        out = os.path.join(self.dir, "bb.musicxml")
        r = call("notation.transpose", {"file": WALTZ, "to": "Bb", "write_musicxml": out})["data"]
        self.assertEqual(r["musicxml_path"], out)
        back = musicxml.import_musicxml(out)
        self.assertEqual((back["key"], back["key_fifths"]), ("A", 3))
        self.assertEqual(back["time_signature"], {"beats": 3, "beat_type": 4})
        self.assertEqual(back["tempo"], 96.0)
        self.assertEqual(back["pickup_qn"], 1.0)
        self.assertEqual(back["melody"]["notes"], r["melody"]["notes"])
        self.assertEqual(back["chords"], r["chords"])

    def test_writer_splits_notes_across_barlines_with_ties(self):
        notes = [{"pitch": 60, "start_qn": 2.0, "length_qn": 3.0}, {"pitch": 64, "start_qn": 5.0, "length_qn": 0.5}]
        xml, warnings = transpose.write_musicxml(notes, 0, "major", {"beats": 4, "beat_type": 4})
        self.assertEqual(warnings, [])
        back = musicxml.parse_score(xml.encode())
        self.assertEqual(back["melody"]["notes"], notes)
        self.assertEqual(xml.count('<tie type="start"/>'), 1)

    def test_pipe_import_into_transpose(self):
        imported = call("notation.import", {"file": WALTZ, "expand_repeats": False})["data"]
        r = call("notation.transpose", {"notes": imported["melody"]["notes"], "key": imported["key"], "mode": imported["mode"], "to": "Bb"})["data"]
        self.assertEqual((r["key"], r["key_fifths"]), ("A", 3))
        self.assertEqual(len(r["melody"]["notes"]), len(imported["melody"]["notes"]))


def _script(path, body):
    with open(path, "w") as f:
        f.write("#!%s\n%s" % (sys.executable, body))
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


class EngraveTests(Base):
    def fake_bin(self, safe=True, fail=False):
        bindir = os.path.join(self.dir, "bin")
        os.makedirs(bindir, exist_ok=True)
        self.log = os.path.join(self.dir, "log.txt")
        _script(os.path.join(bindir, "musicxml2ly"), '''import sys, shutil
a = sys.argv[1:]
shutil.copy(a[-1], %r)
open(a[a.index("-o") + 1], "w").write('\\\\version "2.24.0"\\n')
''' % (self.log + ".xml"))
        _script(os.path.join(bindir, "lilypond"), '''import sys
a = sys.argv[1:]
if "--help" in a:
    print("Usage: lilypond [OPTION]... FILE...\\n  -dBACKEND=svg%s")
    sys.exit(0)
open(%r, "a").write(" ".join(a) + "\\n")
if %r:
    print("fatal error: fake failure"); sys.exit(1)
base = a[a.index("-o") + 1]
if "-dbackend=svg" in a:
    open(base + ".svg", "w").write("<svg/>")
else:
    open(base + ".pdf", "wb").write(b"%%PDF-1.4\\n%%fake\\n")
''' % ("\\n  -dsafe" if safe else "", self.log, fail))
        patcher = mock.patch.dict(os.environ, {"PATH": bindir + os.pathsep + os.environ.get("PATH", "")})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_pdf_through_a_fake_lilypond(self):
        self.fake_bin()
        out = os.path.join(self.dir, "score.pdf")
        r = call("notation.engrave", {"file": WALTZ, "engine": "lilypond", "format": "pdf", "output": out})
        self.assertTrue(r["ok"], r)
        self.assertEqual((r["data"]["path"], r["data"]["engine"], r["data"]["pages"]), (out, "lilypond", 1))
        self.assertGreater(r["data"]["bytes"], 0)
        with open(out, "rb") as f:
            self.assertTrue(f.read().startswith(b"%PDF"))
        with open(self.log) as f:
            line = f.read()
        self.assertIn("-dsafe", line)
        self.assertIn("--pdf", line)

    def test_safe_flag_only_when_the_version_lists_it(self):
        self.fake_bin(safe=False)
        self.assertTrue(call("notation.engrave", {"file": WALTZ, "output": os.path.join(self.dir, "x.pdf")})["ok"])
        with open(self.log) as f:
            self.assertNotIn("safe", f.read())

    def test_svg_and_default_output_directory(self):
        self.fake_bin()
        r = call("notation.engrave", {"file": WALTZ, "engine": "lilypond", "format": "svg"})["data"]
        self.assertEqual(r["path"], os.path.join(self.dir, "rovox-skills-out", "waltz_g.svg"))
        self.assertTrue(os.path.exists(r["path"]))
        self.assertEqual(r["format"], "svg")

    def test_transpose_for_hands_a_transposed_score_to_the_engine(self):
        self.fake_bin()
        r = call("notation.engrave", {"file": WALTZ, "transpose_for": "Bb", "output": os.path.join(self.dir, "bb.pdf")})
        self.assertTrue(r["ok"], r)
        with open(self.log + ".xml", encoding="utf-8") as f:
            sent = f.read()
        self.assertIn("<fifths>3</fifths>", sent)
        self.assertIn("<root-step>A</root-step>", sent)

    def test_engine_failure_is_reported_not_raised(self):
        self.fake_bin(fail=True)
        r = call("notation.engrave", {"file": WALTZ, "output": os.path.join(self.dir, "y.pdf")})
        self.assertFalse(r["ok"])
        self.assertEqual(r["error"]["code"], "SKILL_FAILED")
        self.assertIn("fake failure", r["error"]["message"])

    def test_missing_engines_are_skill_unavailable(self):
        empty = os.path.join(self.dir, "empty")
        os.makedirs(empty)
        with mock.patch.dict(os.environ, {"PATH": empty}), mock.patch("importlib.util.find_spec", return_value=None):
            r = call("notation.engrave", {"file": WALTZ, "engine": "lilypond", "output": os.path.join(self.dir, "z.pdf")})
            self.assertEqual(r["error"]["code"], "SKILL_UNAVAILABLE")
            self.assertIn("lilypond", r["error"]["message"])
            r = call("notation.engrave", {"file": WALTZ, "engine": "verovio", "format": "svg"})
            self.assertEqual(r["error"]["code"], "SKILL_UNAVAILABLE")
            self.assertIn("notation", r["error"]["message"])
            r = call("notation.engrave", {"file": WALTZ})              # default engine for pdf is lilypond
            self.assertEqual(r["error"]["code"], "SKILL_UNAVAILABLE")

    def test_verovio_cannot_make_pdf_and_bad_input_beats_missing_engine(self):
        self.assertEqual(call("notation.engrave", {"file": WALTZ, "engine": "verovio", "format": "pdf"})["error"]["code"], "INVALID_PARAMS")
        empty = os.path.join(self.dir, "empty")
        os.makedirs(empty)
        with mock.patch.dict(os.environ, {"PATH": empty}):
            r = call("notation.engrave", {"file": self.write("bad.xml", "<oops")})
            self.assertEqual(r["error"]["code"], "INVALID_PARAMS")

    def test_output_outside_the_allowed_directories_is_refused(self):
        self.fake_bin()
        r = call("notation.engrave", {"file": WALTZ, "output": "/usr/local/evil.pdf"})
        self.assertEqual(r["error"]["code"], "INVALID_PARAMS")


def _smf():
    def vlq(n):
        out = [n & 0x7F]
        while n >> 7:
            n >>= 7
            out.append((n & 0x7F) | 0x80)
        return bytes(reversed(out))

    def track(events):
        body = b"".join(events) + b"\x00\xff\x2f\x00"
        return b"MTrk" + struct.pack(">I", len(body)) + body
    meta = track([b"\x00\xff\x51\x03" + (500000).to_bytes(3, "big"), b"\x00\xff\x58\x04\x03\x02\x18\x08"])
    notes = track([b"\x00\x90\x3c\x64", vlq(480) + b"\x80\x3c\x00", b"\x00\x90\x3e\x64", vlq(480) + b"\x80\x3e\x00"])
    return b"MThd" + struct.pack(">IHHH", 6, 1, 2, 480) + meta + notes


class AbcTests(Base):
    TUNE = "X:1\nT:Test tune\nM:3/4\nL:1/4\nQ:1/4=100\nK:Em\nCD|\n"

    def test_header_reader(self):
        h = abc.read_header("%abc\nX: 3\nT: Reel\nT: second\nM: C|\nL: 1/8\nK: Bb dor\nabc|\n")
        self.assertEqual((h["X"], h["T"], h["M"], h["K"]), ("3", "Reel", "C|", "Bb dor"))
        self.assertEqual(abc.parse_meter("C|"), (2, 2))
        self.assertEqual(abc.parse_tempo('"Allegro" 1/4=120'), 120.0)
        self.assertEqual(abc.parse_tempo("1/8=120"), 60.0)
        self.assertEqual(abc.parse_key("Em"), ("E", "minor", 1))
        self.assertEqual(abc.parse_key("G"), ("G", "major", 1))
        self.assertEqual(abc.parse_key("D dor")[2], 0)
        self.assertEqual(abc.parse_key("none"), (None, None, None))
        for bad in ("T:x\nK:C\n", "X:1\nT:x\n"):
            with self.assertRaises(NotationError):
                abc.read_header(bad)

    def test_missing_abc2midi_is_skill_unavailable_but_a_bad_header_is_invalid(self):
        path = self.write("t.abc", self.TUNE)
        empty = os.path.join(self.dir, "empty")
        os.makedirs(empty)
        with mock.patch.dict(os.environ, {"PATH": empty}):
            r = call("notation.import", {"file": path})
            self.assertEqual(r["error"]["code"], "SKILL_UNAVAILABLE")
            self.assertIn("abcmidi", r["error"]["message"])
            self.assertEqual(call("notation.import", {"file": self.write("u.abc", "T:no ref\nK:C\n")})["error"]["code"], "INVALID_PARAMS")

    def test_import_through_a_fake_abc2midi(self):
        bindir = os.path.join(self.dir, "bin")
        os.makedirs(bindir)
        _script(os.path.join(bindir, "abc2midi"), 'import sys\nopen(sys.argv[sys.argv.index("-o") + 1], "wb").write(bytes.fromhex(%r))\n' % _smf().hex())
        with mock.patch.dict(os.environ, {"PATH": bindir + os.pathsep + os.environ.get("PATH", "")}):
            r = call("notation.import", {"file": self.write("t.abc", self.TUNE)})
        self.assertTrue(r["ok"], r)
        d = r["data"]
        self.assertEqual(d["melody"]["notes"], [{"pitch": 60, "start_qn": 0.0, "length_qn": 1.0}, {"pitch": 62, "start_qn": 1.0, "length_qn": 1.0}])
        self.assertEqual((d["key"], d["mode"], d["key_fifths"]), ("E", "minor", 1))
        self.assertEqual((d["time_signature"], d["tempo"], d["title"], d["format"]), ({"beats": 3, "beat_type": 4}, 100.0, "Test tune", "abc"))

    def test_smf_reader_rejects_garbage(self):
        with self.assertRaises(Exception):
            abc.read_smf(b"not midi")
        self.assertEqual(abc.read_smf(_smf())[1:3], (120.0, (3, 4)))


class ServerTests(Base):
    def test_tools_listed_with_strict_schemas_and_the_sidecar_still_starts(self):
        r = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/list"}, lambda n: None)
        tools = {t["name"]: t for t in r["result"]["tools"]}
        for name in ("skill.list", "skill.run", "analysis.tempo_map", "notation.import", "notation.transpose", "notation.engrave"):
            self.assertIn(name, tools)
        for name in ("notation.import", "notation.transpose", "notation.engrave"):
            schema = tools[name]["inputSchema"]
            self.assertEqual(schema["type"], "object")
            self.assertTrue(tools[name]["description"])
            self.assertTrue(set(schema["required"]) <= set(schema["properties"]))

            def walk(props):
                for key, sub in props.items():
                    self.assertIsInstance(sub, dict, "%s.%s" % (name, key))
                    walk(sub.get("properties", {}))
            walk(schema["properties"])
        # no external program and no extra: initialize and tools/list still work
        empty = os.path.join(self.dir, "empty")
        os.makedirs(empty)
        with mock.patch.dict(os.environ, {"PATH": empty}):
            r = server.handle({"jsonrpc": "2.0", "id": 2, "method": "initialize", "params": {}}, lambda n: None)
            self.assertEqual(r["result"]["serverInfo"]["name"], "rovox-skills")

    def test_secrets_never_appear_in_responses(self):
        secret = "sk-test-0123456789abcdef"
        with open(WALTZ) as f:
            text = f.read().replace("Waltz in G", secret)
        with mock.patch.dict(os.environ, {"TEST_API_KEY": secret}):
            r = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "notation.import", "arguments": {"file": self.write("s.xml", text)}}}, lambda n: None)
        blob = json.dumps(r)
        self.assertNotIn(secret, blob)
        self.assertIn("[redacted]", blob)

    def test_bad_argument_types_do_not_raise(self):
        for args in ({"file": 5}, {"file": WALTZ, "part": {"x": 1}}, {"file": WALTZ, "write_to": 3}, {"file": WALTZ, "format": "pdf"}):
            r = call("notation.import", args)
            self.assertFalse(r["ok"], args)
            self.assertEqual(r["error"]["code"], "INVALID_PARAMS")
        self.assertFalse(server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "notation.import", "arguments": [1]}}, lambda n: None)["result"]["structuredContent"]["ok"])


if __name__ == "__main__":
    unittest.main()
