#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Reads the licences of the installed LV2 plugins from the Debian packages that ship them (nothing is guessed).

For every <lv2-dir>/*.lv2 bundle: `dpkg -S` gives the package, /usr/share/doc/<package>/copyright (DEP-5) gives the licence of the
`Files: *` stanza and the upstream source. The result is written as JSON:

    { "<bundle>.lv2": {"license": "GPL-2.0-or-later", "source_url": "https://...", "package": "calf-plugins"},
      "lv2:<plugin uri>": { ... same ... } }

A bundle whose package has no copyright file, no `Files: *` stanza or a licence name that is not recognised gets no entry (the catalog
then reports server_use_ok "unknown"). Standard library only; run at image build time by docker/Dockerfile.
"""
import argparse
import json
import os
import re
import subprocess
import sys

# Debian short names (DEP-5 / common-licenses) -> SPDX ids. "+" means "or later".
_SIMPLE = {
    "mit": "MIT", "expat": "MIT", "isc": "ISC", "zlib": "Zlib", "mpl-2.0": "MPL-2.0", "apache-2.0": "Apache-2.0",
    "cc-by-4.0": "CC-BY-4.0", "cc0-1.0": "CC0-1.0", "0bsd": "0BSD",
    "bsd-2-clause": "BSD-2-Clause", "bsd-3-clause": "BSD-3-Clause", "bsd-4-clause": "BSD-4-Clause", "bsd": None,
}
# SPDX ids that DEP-5 files also use as they are.
_SIMPLE.update({i.lower(): i for i in (
    "GPL-2.0-only", "GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later", "LGPL-2.0-only", "LGPL-2.0-or-later", "LGPL-2.1-only",
    "LGPL-2.1-or-later", "LGPL-3.0-only", "LGPL-3.0-or-later", "AGPL-3.0-only", "AGPL-3.0-or-later")})
# Debian ("GPL-2+") and SPDX ("GPL-2.0-or-later", "LGPL-3.0-only") spellings of the GNU licences.
_GNU = re.compile(r"^(a?gpl|lgpl)-(\d(?:\.\d)?)(\+|-or-later|-only)?$", re.IGNORECASE)

# Packages whose samples / data ship in a separate package: the licence of the plugin is the licence of both ({package: [extra packages]}).
# avldrums.lv2 is the plugin; its drum samples are in avldrums.lv2-soundfont and have their own licence.
_EXTRA_PACKAGES = {"avldrums.lv2": ["avldrums.lv2-soundfont"]}


def _token(name):
    """One licence name -> SPDX id, or None if it is not recognised."""
    key = name.strip().lower()
    if key in _SIMPLE:
        return _SIMPLE[key]
    m = _GNU.match(key)
    if m is None:
        return None
    family, version, suffix = m.group(1).upper(), m.group(2), (m.group(3) or "").lower()
    later = suffix in ("+", "-or-later")
    if family == "AGPL":
        if version not in ("3", "3.0"):
            return None
        return "AGPL-3.0-or-later" if later else "AGPL-3.0-only"
    if "." not in version:
        version += ".0"
    if version not in ("2.0", "3.0") and not (family == "LGPL" and version == "2.1"):
        return None
    return "%s-%s-%s" % (family, version, "or-later" if later else "only")


def normalise_license(text):
    """Debian licence expression ("GPL-2+", "GPL-2+ or Expat", "LGPL-2.1+ and MIT") -> SPDX expression, or None if any part is unknown."""
    if not text:
        return None
    words = text.replace(",", " ").replace("(", " ").replace(")", " ").split()
    out = []
    expect_name = True
    for w in words:
        low = w.lower()
        if low in ("or", "and"):
            if expect_name:
                return None
            out.append(low.upper())
            expect_name = True
            continue
        if not expect_name:
            return None         # "with an exception", free text: do not guess
        spdx = _token(w)
        if spdx is None:
            return None
        out.append(spdx)
        expect_name = False
    if expect_name or not out:
        return None
    return " ".join(out)


def _fields(paragraph):
    """DEP-5 paragraph -> {lowercase field: value}; continuation lines are joined with newlines."""
    fields, current = {}, None
    for line in paragraph.splitlines():
        if line[:1] in (" ", "\t"):
            if current is not None:
                fields[current] += "\n" + line.strip()
        elif ":" in line:
            name, _, value = line.partition(":")
            current = name.strip().lower()
            fields[current] = value.strip()
    return fields


def parse_copyright(text):
    """Contents of a Debian copyright file -> (spdx licence or None, source url or None) of the `Files: *` stanza."""
    paragraphs = [p for p in re.split(r"\n\s*\n", text.replace("\r\n", "\n")) if p.strip()]
    source = None
    license_ = None
    for index, paragraph in enumerate(paragraphs):
        fields = _fields(paragraph)
        if index == 0 and "format" in fields:
            src = fields.get("source", "").split()
            source = next((w for w in src if w.startswith(("http://", "https://"))), None)
            continue
        if fields.get("files", "").split() == ["*"]:
            license_ = normalise_license(fields.get("license", "").split("\n")[0])
            break
    return license_, source


_LV2_CORE = "http://lv2plug.in/ns/lv2core#"
_RDF_TYPE = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"
# tokens of Turtle that must not be looked into: <iri>, "string", the statement terminator (a '.' before whitespace / end), anything else
_TTL_TOKEN = re.compile(r'<[^>]*>|"(?:[^"\\]|\\.)*"|\.(?=\s|$)|[^<".]+|.', re.DOTALL)
_TTL_PREFIX = re.compile(r"(?:@prefix|PREFIX)\s+([\w.-]*):\s*<([^>]*)>\s*\.?", re.IGNORECASE)


def _strip_comments(text):
    """Removes '#' comments but keeps '#' inside <iri> and "strings"."""
    return re.sub(r'(<[^>\s]*>|"(?:[^"\\]|\\.)*")|#[^\n]*', lambda m: m.group(1) or "", text)


def _ttl_statements(text):
    """Turtle text (comments already removed) -> list of statement strings (strings blanked, split at the terminating '.')."""
    statements, current = [], []
    for token in _TTL_TOKEN.findall(text):
        if token.startswith('"'):
            current.append('""')
        elif token == ".":
            statements.append("".join(current))
            current = []
        else:
            current.append(token)
    statements.append("".join(current))
    return [s.strip() for s in statements if s.strip()]


def plugin_uris_from_turtle(ttl):
    """URIs of the subjects typed as an LV2 plugin class (lv2:Plugin or a subclass such as lv2:CompressorPlugin) in a Turtle document."""
    ttl = _strip_comments(ttl)
    prefixes = {"lv2": _LV2_CORE, "rdf": _RDF_TYPE.rsplit("type", 1)[0]}      # the usual ones, in case a manifest does not declare them
    prefixes.update({name: iri for name, iri in _TTL_PREFIX.findall(ttl)})

    def expand(term):
        term = term.strip()
        if term.startswith("<") and term.endswith(">"):
            return term[1:-1]
        prefix, sep, local = term.partition(":")
        return prefixes[prefix] + local if sep and prefix in prefixes else None

    def is_plugin_class(iri):
        return bool(iri) and iri.startswith(_LV2_CORE) and iri.endswith("Plugin")

    uris = set()
    for statement in _ttl_statements(_TTL_PREFIX.sub(" ", ttl)):
        m = re.match(r"(<[^>]*>|[^\s;]+)\s*(.*)", statement, re.DOTALL)
        if m is None:
            continue
        subject = expand(m.group(1))
        if not subject or m.group(1).startswith(("@", "[")):
            continue
        for segment in m.group(2).split(";"):      # predicate-object pairs: `a` need not be the first
            parts = segment.split(None, 1)
            if len(parts) != 2 or not (parts[0] == "a" or expand(parts[0]) == _RDF_TYPE):
                continue
            if any(is_plugin_class(expand(obj)) for obj in parts[1].split(",")):
                uris.add(subject)
    return sorted(uris)


def bundle_uris(bundle_dir):
    """Plugin URIs declared in the bundle's manifest.ttl."""
    try:
        with open(os.path.join(bundle_dir, "manifest.ttl"), encoding="utf-8", errors="replace") as f:
            ttl = f.read()
    except OSError:
        return []
    return plugin_uris_from_turtle(ttl)


def _run(args):
    try:
        return subprocess.run(args, capture_output=True, text=True, check=False).stdout
    except OSError:
        return ""


def package_of(path):
    """Debian package that owns `path` (dpkg -S), or None."""
    for line in _run(["dpkg", "-S", path]).splitlines():
        head, sep, _ = line.partition(": ")
        if sep:
            return head.split(",")[0].strip().split(":")[0]
    return None


def homepage_of(package):
    value = _run(["dpkg-query", "-W", "-f=${Homepage}", package]).strip()
    return value if value.startswith(("http://", "https://")) else None


def _package_license(doc_dir, package):
    """(spdx licence or None, source url or None) from /usr/share/doc/<package>/copyright."""
    try:
        with open(os.path.join(doc_dir, package, "copyright"), encoding="utf-8", errors="replace") as f:
            return parse_copyright(f.read())
    except OSError:
        return None, None


def _group(expression):
    return "(%s)" % expression if " OR " in expression else expression


def collect(lv2_dir, doc_dir, package_lookup=package_of, homepage_lookup=homepage_of):
    result = {}
    if not os.path.isdir(lv2_dir):
        return result
    for name in sorted(os.listdir(lv2_dir)):
        bundle = os.path.join(lv2_dir, name)
        if not name.endswith(".lv2") or not os.path.isdir(bundle):
            continue
        package = package_lookup(bundle)
        if not package:
            continue
        license_, source = _package_license(doc_dir, package)
        if license_ is None:
            continue
        for extra in _EXTRA_PACKAGES.get(package, []):
            extra_license, _ = _package_license(doc_dir, extra)
            if extra_license is None:
                license_ = None             # the samples' licence is unknown: so is the whole plugin's
                break
            if extra_license != license_:
                license_ = "%s AND %s" % (_group(license_), _group(extra_license))
        if license_ is None:
            continue
        entry = {"license": license_, "package": package}
        source = source or homepage_lookup(package)
        if source:
            entry["source_url"] = source
        result[name] = entry
        for uri in bundle_uris(bundle):
            result["lv2:" + uri] = entry
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lv2-dir", default="/usr/lib/lv2")
    ap.add_argument("--doc-dir", default="/usr/share/doc")
    ap.add_argument("--out", default="/usr/share/rovox/licenses.json")
    args = ap.parse_args(argv)
    data = collect(args.lv2_dir, args.doc_dir)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, sort_keys=True)
    print("plugin-licenses: %d entries -> %s" % (len(data), args.out), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
