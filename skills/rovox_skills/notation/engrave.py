# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Engraving of MusicXML: LilyPond (musicxml2ly + lilypond as separate processes) for PDF/SVG, or the optional verovio Python module for SVG.

Neither engine is bundled: a missing one gives SKILL_UNAVAILABLE naming what to install. The result is written inside an allowed directory (see paths.py)
and returned as {path, bytes}.
"""
import glob
import importlib.util
import os
import shutil
import subprocess
import tempfile

from . import musicxml, transpose
from .errors import NotationError, invalid, unavailable
from .paths import resolve_in, resolve_out

TIMEOUT_S = 120
MAX_OUTPUT = 200 * 1024 * 1024


def lilypond_available():
    return shutil.which("musicxml2ly") is not None and shutil.which("lilypond") is not None


def verovio_available():
    return importlib.util.find_spec("verovio") is not None


def _run(argv, cwd, timeout=TIMEOUT_S):
    env = {"PATH": os.environ.get("PATH", ""), "HOME": cwd, "LANG": "C.UTF-8"}
    try:
        return subprocess.run(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise NotationError("SKILL_FAILED", "%s did not finish in %d s" % (os.path.basename(argv[0]), timeout))
    except OSError as e:
        raise unavailable("%s could not be started: %s" % (os.path.basename(argv[0]), e.strerror))


def _fail(argv, proc):
    tail = proc.stdout.decode(errors="replace").strip()[-400:]
    return NotationError("SKILL_FAILED", "%s failed (exit %d): %s" % (os.path.basename(argv[0]), proc.returncode, tail or "no output"))


def _safe_flag(lilypond, cwd):
    """LilyPond's safe mode switch, only when this version lists it in --help."""
    proc = _run([lilypond, "--help"], cwd, timeout=30)
    text = proc.stdout.decode(errors="replace")
    if "-dsafe" in text:
        return ["-dsafe"]
    if "--safe" in text:
        return ["--safe"]
    return []


def _lilypond(xml, fmt):
    musicxml2ly, lilypond = shutil.which("musicxml2ly"), shutil.which("lilypond")
    if not musicxml2ly or not lilypond:
        raise unavailable("LilyPond engraving needs musicxml2ly and lilypond (apt package lilypond, part of the skills-notation image), which are not installed")
    with tempfile.TemporaryDirectory(prefix="rovox-ly-") as tmp:
        with open(os.path.join(tmp, "score.xml"), "wb") as f:
            f.write(xml)
        argv = [musicxml2ly, "-o", "score.ly", "score.xml"]
        proc = _run(argv, tmp)
        if proc.returncode != 0 or not os.path.exists(os.path.join(tmp, "score.ly")):
            raise _fail(argv, proc)
        argv = [lilypond] + _safe_flag(lilypond, tmp) + (["--pdf"] if fmt == "pdf" else ["-dbackend=svg"]) + ["-o", "score", "score.ly"]
        proc = _run(argv, tmp)
        if fmt == "pdf":
            files = [os.path.join(tmp, "score.pdf")] if os.path.exists(os.path.join(tmp, "score.pdf")) else []
        else:
            files = sorted(glob.glob(os.path.join(tmp, "score*.svg")))
        if proc.returncode != 0 or not files:
            raise _fail(argv, proc)
        return [_read(f) for f in files]


def _read(path):
    if os.path.getsize(path) > MAX_OUTPUT:
        raise NotationError("SKILL_FAILED", "the engraved file is larger than %d MB" % (MAX_OUTPUT // 1048576))
    with open(path, "rb") as f:
        return f.read()


def _verovio(xml):
    try:
        import verovio
    except ImportError:
        raise unavailable("SVG engraving with verovio needs the optional extra: pip install 'rovox-skills[notation]'")
    tk = verovio.toolkit()
    if not tk.loadData(xml.decode("utf-8", errors="replace")):
        raise NotationError("SKILL_FAILED", "verovio could not load the score")
    return [tk.renderToSVG(page).encode("utf-8") for page in range(1, tk.getPageCount() + 1)]


def tool_engrave(args):
    fmt = args.get("format", "pdf")
    if fmt not in ("pdf", "svg"):
        raise invalid("format must be pdf or svg", element="format")
    engine = args.get("engine")
    if engine is None:
        engine = "lilypond" if fmt == "pdf" or not verovio_available() else "verovio"
    if engine not in ("lilypond", "verovio"):
        raise invalid("engine must be lilypond or verovio", element="engine")
    if engine == "verovio" and fmt == "pdf":
        raise invalid("verovio renders SVG only; use engine lilypond for PDF", element="format")
    path = resolve_in(args.get("file"))
    if path.lower().endswith(".abc"):
        raise invalid("engraving needs MusicXML; import the ABC tune and write it as MusicXML first", element="file")
    data = musicxml.read_score_bytes(path)
    warnings = []
    if args.get("transpose_for") is not None:
        semis, _ = transpose.resolve_target(args["transpose_for"], args.get("octave", 0))
        info = musicxml.parse_score(data, part=args.get("part"), expand_repeats=False)
        xml, warnings = transpose.score_to_musicxml(transpose.transpose_score(info, semis))
        data = xml.encode("utf-8")
    else:
        musicxml.parse_xml(data)            # a clean INVALID_PARAMS for a broken file before an engine is started
    if engine == "lilypond" and not lilypond_available():
        raise unavailable("LilyPond engraving needs musicxml2ly and lilypond (apt package lilypond, part of the skills-notation image), which are not installed")
    if engine == "verovio" and not verovio_available():
        raise unavailable("SVG engraving with verovio needs the optional extra: pip install 'rovox-skills[notation]'")
    pages = _lilypond(data, fmt) if engine == "lilypond" else _verovio(data)
    stem = os.path.splitext(os.path.basename(path))[0] or "score"
    out = resolve_out(args.get("output"), "%s.%s" % (stem, fmt))
    base, ext = os.path.splitext(out)
    written = []
    for i, blob in enumerate(pages):
        target = out if i == 0 else "%s-%d%s" % (base, i + 1, ext)
        with open(target, "wb") as f:
            f.write(blob)
        written.append(target)
    return {"path": written[0], "bytes": len(pages[0]), "engine": engine, "format": fmt, "pages": len(pages), "files": written, "warnings": warnings}
