# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Chords, keys, voicings and a basic harmoniser.

Chord symbols (root C..B with # or b, optional /bass): "" (major triad), m, dim, aug, sus4, sus2, 7, maj7, m7, m7b5, dim7, 6, m6, 9, m9, maj9, add9, madd9,
plus the aliases min, maj, M7, min7, mi7, -, o, +. Anything else degrades to the major (or, for a symbol starting with m, minor) triad with a warning.
All pitches are MIDI numbers (60 = C4), pitch classes are 0..11 (0 = C).
"""
import re

SHARP_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FLAT_NAMES = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
_LETTER = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

# suffix -> intervals above the root, in the order root, third, fifth, seventh/sixth/ninth (the "tones" the style patterns refer to)
QUALITIES = {
    "": (0, 4, 7), "m": (0, 3, 7), "dim": (0, 3, 6), "aug": (0, 4, 8), "sus4": (0, 5, 7), "sus2": (0, 2, 7),
    "7": (0, 4, 7, 10), "maj7": (0, 4, 7, 11), "m7": (0, 3, 7, 10), "m7b5": (0, 3, 6, 10), "dim7": (0, 3, 6, 9),
    "6": (0, 4, 7, 9), "m6": (0, 3, 7, 9), "9": (0, 4, 7, 10, 14), "m9": (0, 3, 7, 10, 14), "maj9": (0, 4, 7, 11, 14),
    "add9": (0, 4, 7, 14), "madd9": (0, 3, 7, 14),
}
ALIASES = {"min": "m", "-": "m", "maj": "", "M": "", "M7": "maj7", "min7": "m7", "mi7": "m7", "-7": "m7", "o": "dim", "o7": "dim7", "+": "aug",
           "min9": "m9", "M9": "maj9", "min6": "m6"}
_CHORD_RE = re.compile(r"^([A-G])([#b]?)([^/]*)(?:/([A-G])([#b]?))?$")
_KEY_RE = re.compile(r"^\s*([A-G])([#b]?)\s*(m|min|minor|major|maj)?\s*$")
_FLAT_MAJOR_TONICS = {5, 10, 3, 8, 1, 6}


def _pc(letter, accidental):
    return (_LETTER[letter] + {"#": 1, "b": -1, "": 0}[accidental]) % 12


def pc_name(pc, flats=False):
    return (FLAT_NAMES if flats else SHARP_NAMES)[pc % 12]


def parse_chord(symbol):
    """Parses a chord symbol into {symbol, root, bass, quality, intervals, pcs, warning}; `bass` is the pitch class of the lowest note (the root unless it is a slash chord)."""
    text = str(symbol).strip()
    m = _CHORD_RE.match(text)
    if not m:
        raise ValueError("cannot read the chord symbol %r" % symbol)
    root = _pc(m.group(1), m.group(2))
    suffix = ALIASES.get(m.group(3), m.group(3))
    warning = None
    if suffix not in QUALITIES:
        if suffix.startswith("dim"):
            triad, name = "dim", "diminished"
        elif suffix.startswith("m") and not suffix.startswith("maj"):
            triad, name = "m", "minor"
        else:
            triad, name = "", "major"
        warning = "chord %s: extension %r is not supported, played as the %s triad" % (text, suffix, name)
        suffix = triad
    intervals = QUALITIES[suffix]
    bass = _pc(m.group(4), m.group(5)) if m.group(4) else root
    return {"symbol": text, "root": root, "bass": bass, "quality": suffix, "intervals": intervals, "pcs": [(root + i) % 12 for i in intervals], "warning": warning}


def role_pc(chord, role):
    """The pitch class of a style role (root, third, fifth, seventh, octave): `seventh` is the chord's fourth tone (a sixth for 6 chords, the ninth for add9)
    and falls back to the root for triads."""
    pcs = chord["pcs"]
    if role in ("root", "octave"):
        return pcs[0]
    if role == "third":
        return pcs[1]
    if role == "fifth":
        return pcs[2]
    if role == "seventh":
        return pcs[3] if len(pcs) > 3 else pcs[0]
    raise ValueError("unknown chord role %r" % role)


def guide_tones(chord):
    """The third and the seventh (the notes that define the chord quality); a triad has only its third."""
    pcs = chord["pcs"]
    return [pcs[1]] + ([pcs[3]] if len(pcs) > 3 else [])


# -- keys ---------------------------------------------------------------------------------------------------------------
def parse_key(key):
    """'G' / 'Em' / 'F# minor' -> (tonic pitch class, is_minor, flats: whether chord symbols are spelled with flats)."""
    m = _KEY_RE.match(str(key))
    if not m:
        raise ValueError("cannot read the key %r (examples: G, Em, Bb, F#m)" % key)
    tonic = _pc(m.group(1), m.group(2))
    minor = (m.group(3) or "") in ("m", "min", "minor")
    relative_major = (tonic + 3) % 12 if minor else tonic
    return tonic, minor, relative_major in _FLAT_MAJOR_TONICS


def scale_pcs(tonic, minor):
    steps = (0, 2, 3, 5, 7, 8, 10) if minor else (0, 2, 4, 5, 7, 9, 11)
    return [(tonic + s) % 12 for s in steps]


# -- bass and voicings ----------------------------------------------------------------------------------------------------
BASS_LO, BASS_HI = 28, 48       # E1 .. C3
COMP_LO, COMP_HI = 48, 72       # C3 .. C5


def bass_root_pitch(pc):
    """The pitch of a bass note of the given class in E1..D#2 (28..39), so the octaves above stay inside the bass range."""
    return 28 + (pc - 28) % 12


def _clamp_bass(p):
    while p > BASS_HI:
        p -= 12
    while p < BASS_LO:
        p += 12
    return p


def bass_pitch(chord, role, next_chord=None):
    """The bass pitch for a style role. The root role is the slash bass for slash chords; `approach` is a chromatic step into the next chord's bass note
    (the fifth when the bass stays or there is no next chord)."""
    base = bass_root_pitch(chord["bass"])
    if role == "root":
        return base
    if role == "octave":
        return base + 12 if base + 12 <= BASS_HI else base
    if role == "approach":
        if next_chord is None:
            return _clamp_bass(base + 7)
        target = bass_root_pitch(next_chord["bass"])
        if target == base:
            return _clamp_bass(base + 7)
        return _clamp_bass(target - 1 if target > base else target + 1)
    p = base + (role_pc(chord, role) - chord["bass"]) % 12
    if p == base:
        p = base + 12 if base + 12 <= BASS_HI else base
    return p - 12 if p > BASS_HI else p


def _comp_tones(chord, max_tones=None):
    ivs = list(chord["intervals"])
    if max_tones:
        return ivs[:max_tones]
    if len(ivs) > 4:        # 9th chords: drop the fifth, keep root, third, seventh, ninth
        ivs.pop(2)
    return ivs


def _candidates(chord, lo, hi, max_tones=None):
    base = [chord["root"] + i for i in _comp_tones(chord, max_tones)]
    out = []
    for inv in range(len(base)):
        stack = base[inv:] + [p + 12 for p in base[:inv]]
        shift = ((lo - min(stack) + 11) // 12) * 12      # the octave shift that puts the lowest note at or just above lo
        for extra in (0, 12):
            v = sorted(p + shift + extra for p in stack)
            if v[0] >= lo and v[-1] <= hi:
                out.append(v)
    return out


def _distance(prev, new):
    return sum(min(abs(a - b) for b in prev) for a in new) + sum(min(abs(a - b) for b in new) for a in prev)


def voice(chord, previous=None, lo=COMP_LO, hi=COMP_HI, max_tones=None):
    """A voicing (sorted MIDI pitches) of the chord inside lo..hi with the smallest movement from `previous` (common tones stay). Returns (voicing, fitted):
    fitted is False when the range was too narrow and a wider one was used.
    max_tones keeps only the first tones of the chord (3 = the triad)."""
    fitted = True
    cands = _candidates(chord, lo, hi, max_tones)
    if not cands:
        fitted = False
        cands = _candidates(chord, lo, max(hi, lo + 16), max_tones) or _candidates(chord, lo, COMP_HI, max_tones)
    target = lo + 9
    if previous:
        def key(v):
            return (_distance(previous, v), v[-1] - v[0], v[0])
    else:
        def key(v):
            return (abs(sum(v) / len(v) - target), v[-1] - v[0], v[0])
    return min(cands, key=key), fitted


# -- the harmoniser -----------------------------------------------------------------------------------------------------
def _key_chords(tonic, minor, sevenths):
    v = "7" if sevenths else ""
    if minor:
        raw = [("i", tonic, "m"), ("iv", tonic + 5, "m"), ("V", tonic + 7, v), ("VI", tonic + 8, "")]
    else:
        raw = [("I", tonic, ""), ("IV", tonic + 5, ""), ("V", tonic + 7, v), ("vi", tonic + 9, "m")]
    return [(name, root % 12, suffix) for name, root, suffix in raw]


_TRANSITION = {("V", "I"): .25, ("V", "i"): .25, ("IV", "V"): .15, ("iv", "V"): .15, ("I", "IV"): .1, ("i", "iv"): .1, ("I", "V"): .05, ("i", "V"): .05,
               ("V", "vi"): .1, ("V", "VI"): .1, ("vi", "IV"): .1, ("VI", "iv"): .1, ("IV", "I"): .1, ("iv", "i"): .1}


def harmonise(melody, key, time_signature, bars, sevenths=False, threshold=0.6):
    """A basic harmoniser: one chord per bar from {I, IV, V(7), vi} (minor keys: i, iv, V(7), VI), picked by Viterbi over how many melody notes (weighted by
    duration, double on the beat) fall inside the triad, with small bonuses for common progressions, a tonic at the start and the end and a dominant before it.
    It knows nothing about phrasing, passing notes or modulation. Returns (chords [{bar, beat, symbol}], confidence [{bar, confidence}], warnings);
    the confidence of a bar is the share of its melody weight inside the chosen triad (0 for a bar without notes, which holds the previous chord)."""
    tonic, minor, flats = parse_key(key)
    cands = _key_chords(tonic, minor, sevenths)
    num, den = time_signature
    bar_qn = num * 4.0 / den
    pulse = bar_qn / (2 if (num % 3 == 0 and num > 3) else num)
    weights = []
    for b in range(bars):
        w = [0.0] * 12
        lo, hi = b * bar_qn, (b + 1) * bar_qn
        for n in melody:
            s, e = n["start_qn"], n["start_qn"] + n["length_qn"]
            overlap = min(e, hi) - max(s, lo)
            if overlap <= 0:
                continue
            on_beat = lo <= s < hi and abs(((s - lo) / pulse) - round((s - lo) / pulse)) < 1e-6
            w[n["pitch"] % 12] += overlap * (2.0 if on_beat else 1.0)
        weights.append(w)

    def fit(w, cand):
        total = sum(w)
        if total <= 0:
            return 0.0
        third = 3 if cand[2] == "m" else 4
        return sum(w[(cand[1] + i) % 12] for i in (0, third, 7)) / total

    scores = [[fit(w, c) for c in cands] for w in weights]
    emit = [[s + (0.3 if i == 0 and (b == 0 or (b == bars - 1 and bars > 1)) else 0.0) + (0.15 if bars >= 3 and b == bars - 2 and cands[i][0] == "V" else 0.0)
             for i, s in enumerate(row)] for b, row in enumerate(scores)]
    value = [emit[0][:]]
    back = []
    for b in range(1, bars):
        row, ptr = [], []
        for i in range(len(cands)):
            options = []
            for j in range(len(cands)):
                bonus = 0.15 if i == j else _TRANSITION.get((cands[j][0], cands[i][0]), 0.0)
                options.append((value[-1][j] + bonus, -j))
            top = max(options)
            row.append(top[0] + emit[b][i])
            ptr.append(-top[1])
        value.append(row)
        back.append(ptr)
    path = [max(range(len(cands)), key=lambda i: (value[-1][i], -i))]
    for ptr in reversed(back):
        path.append(ptr[path[-1]])
    path.reverse()
    chords, confidence, warnings = [], [], []
    for b in range(bars):
        if sum(weights[b]) <= 0:
            if b > 0:
                path[b] = path[b - 1]       # an empty bar holds the previous chord
            _, root, suffix = cands[path[b]]
            warnings.append("bar %d: no melody, chord guessed (%s)" % (b + 1, pc_name(root, flats) + suffix))
            conf = 0.0
        else:
            _, root, suffix = cands[path[b]]
            conf = round(scores[b][path[b]], 3)
            if conf < threshold:
                warnings.append("bar %d: chord guessed (%s explains only %d%% of the melody)" % (b + 1, pc_name(root, flats) + suffix, round(conf * 100)))
        chords.append({"bar": b + 1, "beat": 1, "symbol": pc_name(root, flats) + suffix})
        confidence.append({"bar": b + 1, "confidence": conf})
    return chords, confidence, warnings
