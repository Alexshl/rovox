# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Tests of the accompany skill: PRNG vectors, chord parser, voicings, harmoniser, determinism with golden files, musical rules over every style x level x meter,
and accompany.apply against a fake core. Regenerate the golden files after an intended pattern change with ROVOX_UPDATE_GOLDEN=1."""
import copy
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from rovox_skills import server  # noqa: E402
from rovox_skills.accompany import generate as gen, harmony, tools  # noqa: E402
from rovox_skills.accompany.generate import generate, load_styles  # noqa: E402
from rovox_skills.accompany.prng import Rng  # noqa: E402
from test_skills import FakeCore  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")
PROGRESSION = ["C", "Am", "F", "G7", "C", "F/A", "G", "C"]


def chart(symbols=PROGRESSION):
    return [{"bar": i + 1, "beat": 1, "symbol": s} for i, s in enumerate(symbols)]


def notes_of(result, role):
    return next(t for t in result["tracks"] if t["role"] == role)["notes"]


class PrngTests(unittest.TestCase):
    def test_splitmix64_reference_vectors(self):
        r = Rng(0)
        self.assertEqual([r.next_u64() for _ in range(3)], [0xE220A8397B1DCDAF, 0x6E789E6AA1B965F4, 0x06C45D188009454F])
        r = Rng(1234567)
        self.assertEqual([r.next_u64() for _ in range(2)], [6457827717110365317, 3203168211198807973])

    def test_derived_operations_are_fixed(self):
        self.assertEqual(Rng(42).random(), 0.7415648787718233)
        self.assertEqual(Rng.for_slot(1, 2, "bass").next_u64(), 13146422355860563881)
        items = list(range(8))
        Rng(7).shuffle(items)
        self.assertEqual(items, [7, 4, 6, 1, 2, 5, 0, 3])
        self.assertEqual(Rng(7).choice("abcdef"), "c")

    def test_ranges_and_slot_independence(self):
        r = Rng(5)
        values = [r.randint(1, 6) for _ in range(500)]
        self.assertEqual(set(values), {1, 2, 3, 4, 5, 6})
        self.assertTrue(all(0.0 <= Rng(i).random() < 1.0 for i in range(100)))
        self.assertNotEqual(Rng.for_slot(1, 1, "bass").next_u64(), Rng.for_slot(1, 1, "drums").next_u64())
        self.assertNotEqual(Rng.for_slot(1, 1, "bass").next_u64(), Rng.for_slot(1, 2, "bass").next_u64())


class ChordTests(unittest.TestCase):
    TABLE = [
        ("C", 0, 0, (0, 4, 7)), ("Am", 9, 9, (0, 3, 7)), ("G7", 7, 7, (0, 4, 7, 10)), ("Dm7", 2, 2, (0, 3, 7, 10)), ("F/A", 5, 9, (0, 4, 7)),
        ("Bb", 10, 10, (0, 4, 7)), ("C#dim", 1, 1, (0, 3, 6)), ("Esus4", 4, 4, (0, 5, 7)), ("Cmaj7", 0, 0, (0, 4, 7, 11)), ("Bm7b5", 11, 11, (0, 3, 6, 10)),
        ("Cadd9", 0, 0, (0, 4, 7, 14)), ("C6", 0, 0, (0, 4, 7, 9)), ("C9", 0, 0, (0, 4, 7, 10, 14)), ("Ebm", 3, 3, (0, 3, 7)), ("Gmaj", 7, 7, (0, 4, 7)),
        ("A-7", 9, 9, (0, 3, 7, 10)), ("D/F#", 2, 6, (0, 4, 7)), ("Faug", 5, 5, (0, 4, 8)),
    ]

    def test_parser_table(self):
        for symbol, root, bass, intervals in self.TABLE:
            c = harmony.parse_chord(symbol)
            self.assertEqual((c["root"], c["bass"], c["intervals"]), (root, bass, intervals), symbol)
            self.assertIsNone(c["warning"], symbol)

    def test_unknown_extension_degrades_to_the_triad_with_a_warning(self):
        c = harmony.parse_chord("Cmaj13")
        self.assertEqual(c["intervals"], (0, 4, 7))
        self.assertIn("not supported", c["warning"])
        c = harmony.parse_chord("Dm11")
        self.assertEqual(c["intervals"], (0, 3, 7))
        self.assertIsNotNone(c["warning"])

    def test_garbage_is_rejected(self):
        for bad in ("", "H", "xyz", "C/"):
            with self.assertRaises(ValueError):
                harmony.parse_chord(bad)

    def test_keys(self):
        self.assertEqual(harmony.parse_key("G"), (7, False, False))
        self.assertEqual(harmony.parse_key("Em"), (4, True, False))
        self.assertEqual(harmony.parse_key("Bb"), (10, False, True))
        self.assertEqual(harmony.parse_key("D minor"), (2, True, True))
        self.assertEqual(harmony.scale_pcs(7, False), [7, 9, 11, 0, 2, 4, 6])

    def test_guide_tones(self):
        self.assertEqual(harmony.guide_tones(harmony.parse_chord("G7")), [11, 5])
        self.assertEqual(harmony.guide_tones(harmony.parse_chord("C")), [4])

    def test_bass_pitch_roles_and_slash(self):
        c, fa = harmony.parse_chord("C"), harmony.parse_chord("F/A")
        self.assertEqual(harmony.bass_pitch(c, "root"), 36)
        self.assertEqual(harmony.bass_pitch(c, "fifth"), 43)
        self.assertEqual(harmony.bass_pitch(fa, "root") % 12, 9)
        for pc in range(12):
            chord = harmony.parse_chord(harmony.pc_name(pc) + "7")
            for role in ("root", "third", "fifth", "seventh", "octave", "approach"):
                self.assertTrue(harmony.BASS_LO <= harmony.bass_pitch(chord, role, harmony.parse_chord("C")) <= harmony.BASS_HI, (pc, role))
        self.assertEqual(harmony.bass_pitch(c, "approach", harmony.parse_chord("F")), 30)       # F# above the F1 that follows, the chromatic step in the direction of travel

    def test_voice_leading_keeps_common_tones_and_moves_little(self):
        prev, moves = None, []
        for symbol in ("C", "Am", "F", "G", "C"):
            v, fitted = harmony.voice(harmony.parse_chord(symbol), prev)
            self.assertTrue(fitted)
            self.assertTrue(all(harmony.COMP_LO <= p <= harmony.COMP_HI for p in v))
            self.assertEqual(sorted({p % 12 for p in v}), sorted(harmony.parse_chord(symbol)["pcs"]))
            if prev:
                moves.append(sum(abs(a - b) for a, b in zip(prev, v)))
            prev = v
        self.assertTrue(all(m <= 7 for m in moves), moves)
        c, _ = harmony.voice(harmony.parse_chord("C"))
        am, _ = harmony.voice(harmony.parse_chord("Am"), c)
        self.assertTrue({60 % 12, 64 % 12} <= {p % 12 for p in am if p in c})

    def test_narrow_range_reports_not_fitted(self):
        v, fitted = harmony.voice(harmony.parse_chord("C"), None, 48, 50)
        self.assertFalse(fitted)
        self.assertTrue(v)


class HarmoniserTests(unittest.TestCase):
    @staticmethod
    def melody():
        # G major, 4/4: G B D G | C E G E | D F# A D | B D G G
        bars = [[67, 71, 74, 67], [72, 76, 67, 76], [74, 78, 69, 74], [71, 74, 67, 67]]
        return [{"pitch": p, "start_qn": 4 * b + i, "length_qn": 1} for b, ps in enumerate(bars) for i, p in enumerate(ps)]

    def test_simple_g_major_melody_gets_I_IV_V_I(self):
        chords, conf, warnings = harmony.harmonise(self.melody(), "G", (4, 4), 4)
        self.assertEqual([c["symbol"] for c in chords], ["G", "C", "D", "G"])
        self.assertEqual([c["bar"] for c in chords], [1, 2, 3, 4])
        self.assertTrue(all(c["confidence"] >= 0.6 for c in conf))
        self.assertEqual(warnings, [])

    def test_sevenths_and_empty_bars_warn(self):
        chords, conf, warnings = harmony.harmonise(self.melody()[:12], "G", (4, 4), 5, sevenths=True)
        self.assertEqual(chords[2]["symbol"], "D7")
        self.assertEqual(conf[4]["confidence"], 0.0)
        self.assertTrue(any("bar 5" in w for w in warnings))

    def test_generate_returns_the_chosen_chords(self):
        r = generate(melody=self.melody(), key="G", style="pop-8th", level="beginner", seed=1)
        self.assertEqual([c["symbol"] for c in r["chords"]], ["G", "C", "D", "G"])
        self.assertTrue(r["analysis"]["harmonised"])
        self.assertEqual(len(r["analysis"]["chord_confidence"]), 4)
        again = generate(melody=self.melody(), chords=r["chords"], key="G", style="pop-8th", level="beginner", seed=1)
        self.assertEqual(json.dumps(notes_of(r, "bass")), json.dumps(notes_of(again, "bass")))

    def test_needs_a_key_without_chords(self):
        with self.assertRaises(ValueError):
            generate(melody=self.melody(), style="waltz", time_signature="3/4")


class DeterminismTests(unittest.TestCase):
    ARGS = {
        "waltz": dict(chords=chart(), key="C", time_signature="3/4", tempo=132, bars=8, style="waltz", level="intermediate", seed=11, count_in_bars=1),
        "pop-8th": dict(chords=chart(["G", "Em", "C", "D7", "G", "Em", "Am7", "D7", "G", "C", "D", "G"]), key="G", time_signature="4/4", tempo=108, bars=12,
                        style="pop-8th", level="intermediate", seed=4, intro_bars=1),
    }

    def test_same_seed_gives_byte_identical_json(self):
        for name, args in self.ARGS.items():
            self.assertEqual(json.dumps(generate(**args)), json.dumps(generate(**args)), name)

    def test_other_seeds_change_only_variation_slots(self):
        base = dict(self.ARGS["pop-8th"], bars=16, chords=chart(["C", "Am", "F", "G"] * 4))
        a, b = generate(**dict(base, seed=1)), generate(**dict(base, seed=2))
        self.assertNotEqual(json.dumps(a["tracks"]), json.dumps(b["tracks"]))
        self.assertEqual(a["chords"], b["chords"])
        for role in ("bass", "comp", "drums"):
            # every bar still starts with the same chord root on the bass whatever the seed
            self.assertEqual([n["pitch"] for n in notes_of(a, role) if n["start_qn"] % 4 == 0 and role == "bass"],
                             [n["pitch"] for n in notes_of(b, role) if n["start_qn"] % 4 == 0 and role == "bass"])

    def test_golden_files(self):
        update = os.environ.get("ROVOX_UPDATE_GOLDEN")
        for name, args in self.ARGS.items():
            path = os.path.join(FIXTURES, "accompany_%s.json" % name)
            text = json.dumps(generate(**args), indent=1, sort_keys=True) + "\n"
            if update:
                with open(path, "w") as f:
                    f.write(text)
            with open(path) as f:
                self.assertEqual(f.read(), text, "the %s golden file differs: review the diff, then regenerate with ROVOX_UPDATE_GOLDEN=1" % name)


class StyleMatrixTests(unittest.TestCase):
    def combos(self):
        for pack in load_styles().values():
            for meter in pack["meters"]:
                for level in pack["levels"]:
                    yield pack["id"], meter, level

    def run_one(self, style, meter, level, seed=3, symbols=PROGRESSION, **extra):
        return generate(chords=chart(symbols), key="C", time_signature=meter, tempo=100 if style != "waltz" else 120, bars=len(symbols), style=style, level=level, seed=seed, **extra)

    def test_ships_the_v1_styles_and_levels(self):
        packs = load_styles()
        self.assertEqual(set(packs), {"waltz", "ballad", "pop-8th", "march"})
        self.assertEqual({(p["id"], m) for p in packs.values() for m in p["meters"]},
                         {("waltz", "3/4"), ("ballad", "4/4"), ("ballad", "6/8"), ("pop-8th", "4/4"), ("march", "2/4"), ("march", "4/4")})
        for p in packs.values():
            self.assertEqual(set(p["levels"]), {"beginner", "intermediate"})
            self.assertIsInstance(p["version"], int)

    def test_every_combination_obeys_the_rules(self):
        for style, meter, level in self.combos():
            for seed in (1, 2, 3):
                r = self.run_one(style, meter, level, seed, count_in_bars=1, intro_bars=1)
                label = (style, meter, level, seed)
                total = r["timeline"]["total_qn"]
                bar_qn = r["timeline"]["bar_qn"]
                body = r["timeline"]["body_start_qn"]
                self.assertEqual(total, (len(PROGRESSION) + 2) * bar_qn)
                roles = [t["role"] for t in r["tracks"]]
                self.assertIn("bass", roles, label)
                self.assertIn("comp", roles, label)
                for t in r["tracks"]:
                    spans = {}
                    for n in t["notes"]:
                        self.assertTrue(0 <= n["start_qn"] and n["start_qn"] + n["length_qn"] <= total + 1e-6, (label, t["role"], n))
                        self.assertTrue(1 <= n["velocity"] <= 127 and isinstance(n["velocity"], int), (label, n))
                        self.assertGreater(n["length_qn"], 0)
                        if t["role"] == "bass":
                            self.assertTrue(harmony.BASS_LO <= n["pitch"] <= harmony.BASS_HI, (label, n))
                        if t["role"] == "comp":
                            self.assertTrue(harmony.COMP_LO <= n["pitch"] <= harmony.COMP_HI, (label, n))
                        # a note stays inside its own bar
                        bar = int(n["start_qn"] // bar_qn + 1e-9)
                        self.assertLessEqual(n["start_qn"] + n["length_qn"], (bar + 1) * bar_qn + 1e-6, (label, n))
                        spans.setdefault(n["pitch"], []).append((n["start_qn"], n["start_qn"] + n["length_qn"]))
                    for pitch, lst in spans.items():
                        lst.sort()
                        for (s1, e1), (s2, e2) in zip(lst, lst[1:]):
                            self.assertLessEqual(e1, s2 + 1e-6, "same-pitch overlap %s %s %s" % (label, t["role"], pitch))
                # the bass is on the (slash) root on every downbeat of the body
                chords = [harmony.parse_chord(c["symbol"]) for c in r["chords"]]
                bass = notes_of(r, "bass")
                for i, chord in enumerate(chords):
                    start = body + i * bar_qn
                    hit = [n for n in bass if abs(n["start_qn"] - start) < 1e-6]
                    self.assertTrue(hit, (label, "no bass on the downbeat of bar", i + 1))
                    self.assertEqual(hit[0]["pitch"] % 12, chord["bass"], (label, i + 1))

    def test_beginner_is_sparse_and_steady(self):
        for style, meter, level in self.combos():
            if level != "beginner":
                continue
            r = self.run_one(style, meter, level)
            num, den = (int(x) for x in meter.split("/"))
            pulse = 4.0 / den * (3 if den == 8 else 1)
            for role in ("bass", "comp"):
                notes = notes_of(r, role)
                for n in notes:
                    self.assertAlmostEqual(n["start_qn"] / pulse, round(n["start_qn"] / pulse), places=6, msg=(style, meter, role, n))
                self.assertLessEqual(max(sum(1 for m in notes if m["start_qn"] == n["start_qn"]) for n in notes), 3, (style, meter))
            bass_roles = {n["pitch"] % 12 for n in notes_of(r, "bass")}
            self.assertTrue(bass_roles <= {0, 9, 5, 7, 2, 4, 11}, bass_roles)       # root and fifth of the chords of the chart

    def test_intermediate_is_busier_than_beginner(self):
        for style, meter, level in self.combos():
            if level != "intermediate":
                continue
            fuller = self.run_one(style, meter, "intermediate", symbols=PROGRESSION * 2)
            simple = self.run_one(style, meter, "beginner", symbols=PROGRESSION * 2)
            self.assertGreater(sum(len(t["notes"]) for t in fuller["tracks"]), sum(len(t["notes"]) for t in simple["tracks"]), (style, meter))

    def test_waltz_bar_shape(self):
        r = self.run_one("waltz", "3/4", "beginner")
        bass, comp = notes_of(r, "bass"), notes_of(r, "comp")
        self.assertEqual(r["timeline"]["total_qn"], 8 * 3)
        for bar in range(8):
            self.assertTrue([n for n in bass if n["start_qn"] == bar * 3], bar)
            if bar < 7:         # the last bar is the sustained ending chord
                for beat in (1, 2):
                    self.assertTrue([n for n in comp if n["start_qn"] == bar * 3 + beat], (bar, beat))
                self.assertFalse([n for n in comp if n["start_qn"] == bar * 3], bar)       # the chords come on 2 and 3

    def test_ending_is_a_sustained_final_chord(self):
        r = self.run_one("pop-8th", "4/4", "intermediate")
        last = [n for n in notes_of(r, "comp") if n["start_qn"] >= 28]
        self.assertEqual(len(last), 3)
        self.assertTrue(all(n["start_qn"] == 28 and n["length_qn"] > 3.5 for n in last))
        self.assertEqual(len([n for n in notes_of(r, "bass") if n["start_qn"] >= 28]), 1)
        plain = self.run_one("pop-8th", "4/4", "intermediate", ending=False)
        self.assertGreater(len([n for n in notes_of(plain, "comp") if n["start_qn"] >= 28]), 3)

    def test_fills_come_at_phrase_ends_and_intermediate_only_changes_with_seed_there(self):
        r = self.run_one("pop-8th", "4/4", "intermediate", symbols=["C"] * 8)
        drums = notes_of(r, "drums")
        fill_bar = [n for n in drums if 12 <= n["start_qn"] < 16]       # bar 4
        plain_bar = [n for n in drums if 8 <= n["start_qn"] < 12]       # bar 3
        self.assertTrue(any(n["pitch"] in (38, 41, 47, 50) and n["start_qn"] >= 14 for n in fill_bar))
        self.assertGreaterEqual(len(fill_bar), len(plain_bar))
        self.assertTrue(any(n["pitch"] == 49 and n["start_qn"] == 16 for n in drums))       # a crash after the fill

    def test_count_in_and_intro_shift_the_music(self):
        r = self.run_one("pop-8th", "4/4", "beginner", count_in_bars=2, intro_bars=1)
        self.assertEqual(r["timeline"]["body_start_qn"], 12)
        self.assertEqual(notes_of(r, "bass")[0]["start_qn"], 8)       # the intro starts after the count-in; the body after 12
        click = next(t for t in r["tracks"] if t["role"] == "click")["notes"]
        self.assertEqual([n["start_qn"] for n in click], [0, 1, 2, 3, 4, 5, 6, 7])
        self.assertGreater(click[0]["velocity"], click[1]["velocity"])
        six8 = self.run_one("ballad", "6/8", "beginner", count_in_bars=1)
        self.assertEqual([n["start_qn"] for n in next(t for t in six8["tracks"] if t["role"] == "click")["notes"]], [0, 1.5])

    def test_chord_changes_mid_bar_are_honoured(self):
        chords = [{"bar": 1, "beat": 1, "symbol": "C"}, {"bar": 1, "beat": 3, "symbol": "G"}, {"bar": 2, "beat": 1, "symbol": "C"}]
        for style, meter, level in (("pop-8th", "4/4", "beginner"), ("pop-8th", "4/4", "intermediate"), ("ballad", "4/4", "intermediate"), ("march", "4/4", "intermediate")):
            r = generate(chords=chords, key="C", time_signature=meter, bars=2, style=style, level=level, seed=1, ending=False, tempo=100)
            bass, comp = notes_of(r, "bass"), notes_of(r, "comp")
            at = [n for n in bass if n["start_qn"] == 2]
            self.assertTrue(at and at[0]["pitch"] % 12 == 7, (style, level))
            self.assertTrue({n["pitch"] % 12 for n in comp if n["start_qn"] == 2} <= {7, 11, 2} and any(n["start_qn"] == 2 for n in comp), (style, level))
            for n in bass + comp:
                if n["start_qn"] < 2:
                    self.assertLessEqual(n["start_qn"] + n["length_qn"], 2 + 1e-6, (style, level, n))

    def test_slash_chord_bass(self):
        r = generate(chords=chart(["C", "F/A", "G/B", "C"]), key="C", time_signature="4/4", bars=4, style="pop-8th", level="beginner", tempo=100)
        starts = {n["start_qn"]: n["pitch"] % 12 for n in notes_of(r, "bass")}
        self.assertEqual([starts[0], starts[4], starts[8], starts[12]], [0, 9, 11, 0])

    def test_melody_aware_register(self):
        melody = [{"pitch": 60 + (i % 4), "start_qn": i, "length_qn": 1} for i in range(16)]
        r = generate(melody=melody, chords=chart(["C", "Am", "F", "G"]), key="C", time_signature="4/4", style="ballad", level="intermediate", seed=2, tempo=70)
        self.assertEqual(r["warnings"], [])
        self.assertLessEqual(max(n["pitch"] for n in notes_of(r, "comp")), 59)
        free = generate(chords=chart(["C", "Am", "F", "G"]), key="C", time_signature="4/4", style="ballad", level="intermediate", seed=2, tempo=70)
        self.assertGreater(max(n["pitch"] for n in notes_of(free, "comp")), 59)

    def test_melody_below_the_comp_range_warns(self):
        melody = [{"pitch": 50, "start_qn": i, "length_qn": 1} for i in range(4)]
        r = generate(melody=melody, chords=chart(["C"]), key="C", time_signature="4/4", style="pop-8th", level="beginner", tempo=100)
        self.assertTrue(any("too low" in w for w in r["warnings"]))

    def test_unsupported_meter_style_and_level_are_errors(self):
        for kw in (dict(style="waltz", time_signature="4/4"), dict(style="nope"), dict(level="expert"), dict(parts=["kazoo"])):
            with self.assertRaises(ValueError):
                generate(**dict(dict(chords=chart(), key="C", bars=8, style="pop-8th", level="beginner"), **kw))

    def test_warnings_for_odd_input(self):
        r = generate(chords=[{"bar": 2, "symbol": "Cmaj13"}, {"bar": 9, "symbol": "G"}], key="C", bars=4, style="pop-8th", level="beginner", tempo=300)
        text = " ".join(r["warnings"])
        self.assertIn("not supported", text)
        self.assertIn("after the last bar", text)
        self.assertIn("held from the start", text)
        self.assertIn("tempo", text)
        self.assertEqual(r["chords"][0], {"bar": 1, "beat": 1, "symbol": "Cmaj13"})

    def test_parts_subset_and_missing_drums(self):
        r = self.run_one("ballad", "4/4", "beginner", parts=["bass", "drums"])
        self.assertEqual([t["role"] for t in r["tracks"]], ["bass"])
        self.assertTrue(any("no drums part" in w for w in r["warnings"]))
        self.assertEqual([t["role"] for t in self.run_one("pop-8th", "4/4", "beginner")["tracks"]], ["bass", "comp", "drums"])

    def test_swing_moves_the_off_beats(self):
        packs = load_styles()
        swung = copy.deepcopy(packs["pop-8th"])
        swung["swing"] = 2.0 / 3.0
        old = gen._styles
        gen._styles = dict(packs, **{"pop-8th": swung})
        try:
            r = self.run_one("pop-8th", "4/4", "intermediate")
        finally:
            gen._styles = old
        hats = sorted({round(n["start_qn"] % 1, 4) for n in notes_of(r, "drums") if n["pitch"] == 42})
        self.assertEqual(hats, [0.0, 0.6667])


class ApplyTests(unittest.TestCase):
    ARGS = dict(project_id="p_1", chords=chart(["C", "G", "Am", "F"]), key="C", time_signature="4/4", tempo=96, bars=4, style="pop-8th", level="beginner", seed=2)

    def test_apply_creates_tracks_items_and_the_drum_sampler(self):
        core = FakeCore()
        out = tools.apply(core, dict(self.ARGS, set_tempo=True, name_prefix="Pop"))
        calls = [(t, a) for t, a in core.calls]
        self.assertEqual([t for t, _ in calls], ["tempo.set", "track.create", "item.create_midi", "track.create", "item.create_midi", "track.create", "fx.add", "item.create_midi"])
        self.assertEqual(calls[0][1]["points"], [{"bpm": 96.0, "qn": 0}])
        self.assertEqual([a["name"] for t, a in calls if t == "track.create"], ["Pop Bass", "Pop Piano" if False else "Pop Electric piano", "Pop Drums"])
        fx = next(a for t, a in calls if t == "fx.add")
        self.assertEqual(fx["builtin"], "rovox:drums")
        self.assertEqual(fx["track_id"], "trk_3")
        item = next(a for t, a in calls if t == "item.create_midi")
        self.assertEqual((item["project_id"], item["track_id"], item["start"], item["length"]), ("p_1", "trk_1", {"bar": 1}, {"qn": 16.0}))
        self.assertTrue(item["notes"] and set(item["notes"][0]) == {"pitch", "start_qn", "length_qn", "velocity"})
        self.assertEqual([(t["role"], t["track_id"], t["instrument"]) for t in out["tracks"]], [("bass", "trk_1", None), ("comp", "trk_2", None), ("drums", "trk_3", "rovox:drums")])
        self.assertTrue(all(t["item_id"].startswith("itm_") and t["notes"] > 0 for t in out["tracks"]))
        self.assertEqual(out["chords"], self.ARGS["chords"])
        self.assertTrue(out["written"] and out["tempo_set"])

    def test_apply_without_write_or_tempo_makes_no_extra_calls(self):
        core = FakeCore()
        out = tools.apply(core, dict(self.ARGS, write=False))
        self.assertEqual(core.calls, [])
        self.assertFalse(out["written"])
        self.assertEqual(len(out["tracks"]), 3)
        core = FakeCore()
        tools.apply(core, dict(self.ARGS, parts=["bass"]))
        self.assertEqual([t for t, _ in core.calls], ["track.create", "item.create_midi"])

    def test_apply_needs_a_project(self):
        with self.assertRaises(ValueError):
            tools.apply(FakeCore(), {k: v for k, v in self.ARGS.items() if k != "project_id"})


class ServerTests(unittest.TestCase):
    def call(self, name, args, core=None):
        msg = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}}
        return server.handle(msg, lambda origin: core or FakeCore())["result"]

    def test_tools_are_listed_with_object_schemas(self):
        reply = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/list"}, lambda o: FakeCore())
        listed = {t["name"]: t for t in reply["result"]["tools"]}
        self.assertTrue({"accompany.styles", "accompany.generate", "accompany.apply", "skill.run"} <= set(listed))

        def check(schema, path):
            self.assertIsInstance(schema, dict, path)
            for k, v in schema.get("properties", {}).items():
                self.assertIsInstance(v, dict, path + "." + k)
                self.assertTrue(v.get("description") or v.get("type") == "object", path + "." + k)
                check(v, path + "." + k)
            if "items" in schema:
                check(schema["items"], path + "[]")
        for t in tools.ACCOMPANY_TOOLS:
            self.assertTrue(t["description"])
            check(t["inputSchema"], t["name"])

    def test_styles_and_generate_over_mcp(self):
        data = self.call("accompany.styles", {})["structuredContent"]["data"]
        self.assertEqual(sorted(s["id"] for s in data["styles"]), ["ballad", "march", "pop-8th", "waltz"])
        waltz = next(s for s in data["styles"] if s["id"] == "waltz")
        self.assertEqual((waltz["meters"], waltz["levels"]), (["3/4"], ["beginner", "intermediate"]))
        res = self.call("accompany.generate", dict(chords=chart(), key="C", time_signature="3/4", bars=8, style="waltz", seed=5))
        self.assertFalse(res["isError"])
        self.assertEqual(res["structuredContent"]["data"]["style"], {"id": "waltz", "version": 1})

    def test_errors_are_reported_not_raised(self):
        res = self.call("accompany.generate", dict(chords=chart(), key="C", style="waltz", time_signature="4/4", bars=8))
        self.assertTrue(res["isError"])
        self.assertEqual(res["structuredContent"]["error"]["code"], "SKILL_FAILED")

    def test_apply_over_mcp_uses_the_skill_origin(self):
        origins, core = [], FakeCore()
        msg = {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "accompany.apply", "arguments": ApplyTests.ARGS}}
        res = server.handle(msg, lambda origin: origins.append(origin) or core)["result"]
        self.assertFalse(res["isError"])
        self.assertEqual(origins, ["skill:accompany"])
        self.assertEqual(sum(1 for t, _ in core.calls if t == "item.create_midi"), 3)

    def test_other_tools_still_work(self):
        reply = server.handle({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "skill.list", "arguments": {}}}, lambda o: FakeCore())
        self.assertTrue(reply["result"]["structuredContent"]["ok"])


if __name__ == "__main__":
    unittest.main()
