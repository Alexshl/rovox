# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Unit tests for tools/plugin-licenses.py (python3 -m unittest discover -s tools/tests)."""
import importlib.util
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("plugin_licenses", os.path.join(HERE, "..", "plugin-licenses.py"))
pl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pl)


def fixture(name):
    with open(os.path.join(HERE, "fixtures", name), encoding="utf-8") as f:
        return f.read()


class Normalise(unittest.TestCase):
    def test_debian_names(self):
        cases = {
            "GPL-2": "GPL-2.0-only", "GPL-2+": "GPL-2.0-or-later", "GPL-3": "GPL-3.0-only", "GPL-3+": "GPL-3.0-or-later",
            "LGPL-2.1+": "LGPL-2.1-or-later", "LGPL-2.1": "LGPL-2.1-only", "LGPL-3+": "LGPL-3.0-or-later", "LGPL-2+": "LGPL-2.0-or-later",
            "ISC": "ISC", "MIT": "MIT", "Expat": "MIT", "BSD-3-clause": "BSD-3-Clause", "Zlib": "Zlib", "AGPL-3+": "AGPL-3.0-or-later",
        }
        for name, spdx in cases.items():
            self.assertEqual(pl.normalise_license(name), spdx, name)

    def test_spdx_names(self):
        cases = {
            "GPL-2.0-or-later": "GPL-2.0-or-later", "gpl-2.0-only": "GPL-2.0-only", "GPL-3.0-only": "GPL-3.0-only", "LGPL-3.0-only": "LGPL-3.0-only",
            "LGPL-2.1-or-later": "LGPL-2.1-or-later", "AGPL-3.0-or-later": "AGPL-3.0-or-later", "AGPL-3.0-only": "AGPL-3.0-only",
            "LGPL-2.0-only": "LGPL-2.0-only", "MPL-2.0": "MPL-2.0", "Apache-2.0": "Apache-2.0", "BSD-2-Clause": "BSD-2-Clause", "CC0-1.0": "CC0-1.0",
        }
        for name, spdx in cases.items():
            self.assertEqual(pl.normalise_license(name), spdx, name)
        self.assertEqual(pl.normalise_license("GPL-2.0-or-later OR MIT"), "GPL-2.0-or-later OR MIT")
        self.assertIsNone(pl.normalise_license("GPL-4.0-only"))
        self.assertIsNone(pl.normalise_license("AGPL-1.0-or-later"))

    def test_expressions(self):
        self.assertEqual(pl.normalise_license("GPL-2+ or Expat"), "GPL-2.0-or-later OR MIT")
        self.assertEqual(pl.normalise_license("LGPL-2.1+ and MIT"), "LGPL-2.1-or-later AND MIT")

    def test_unknown_is_none(self):
        for name in ("custom", "", "GPL", "GPL-2+ with exception", "GPL-2+ or", "or GPL-2+", "BSD", "GPL-4", "AGPL-1"):
            self.assertIsNone(pl.normalise_license(name), name)


class Copyright(unittest.TestCase):
    def test_files_star_stanza_and_source(self):
        self.assertEqual(pl.parse_copyright(fixture("copyright-gpl2plus")),
                         ("LGPL-2.1-or-later", "https://github.com/calf-studio-gear/calf"))

    def test_dual_licence(self):
        self.assertEqual(pl.parse_copyright(fixture("copyright-mixed"))[0], "GPL-2.0-or-later OR MIT")

    def test_unrecognised_licence(self):
        self.assertIsNone(pl.parse_copyright(fixture("copyright-proprietary"))[0])

    def test_no_files_star(self):
        self.assertIsNone(pl.parse_copyright(fixture("copyright-nostar"))[0])

    def test_spdx_licence_in_dep5(self):
        self.assertEqual(pl.parse_copyright(fixture("copyright-spdx")),
                         ("LGPL-3.0-or-later", "https://github.com/lsp-plugins/lsp-plugins"))


class Turtle(unittest.TestCase):
    def test_prefixed_manifest_with_subclasses_and_any_predicate_order(self):
        self.assertEqual(pl.plugin_uris_from_turtle(fixture("manifest-lsp.ttl")),
                         ["http://lsp-plug.in/plugins/lv2/compressor_mono", "http://lsp-plug.in/plugins/lv2/para_equalizer_x16_lr",
                          "http://lsp-plug.in/plugins/lv2/rev"])

    def test_full_iri_class_and_other_prefix_name(self):
        ttl = ("@prefix core: <http://lv2plug.in/ns/lv2core#> .\n"
               "<urn:a> core:binary <a.so> ; a core:Plugin .\n"
               "<urn:b> a <http://lv2plug.in/ns/lv2core#Plugin> .\n"
               "<urn:c> a <http://example.org/ns#Plugin> .\n")
        self.assertEqual(pl.plugin_uris_from_turtle(ttl), ["urn:a", "urn:b"])

    def test_no_prefix_declaration_and_sparql_prefix(self):
        self.assertEqual(pl.plugin_uris_from_turtle("<urn:x>\n  a lv2:Plugin ;\n  rdfs:seeAlso <x.ttl> .\n"), ["urn:x"])
        self.assertEqual(pl.plugin_uris_from_turtle("PREFIX l: <http://lv2plug.in/ns/lv2core#>\n<urn:y> a l:ReverbPlugin ."), ["urn:y"])


class Collect(unittest.TestCase):
    def test_bundles_map_to_packages(self):
        with tempfile.TemporaryDirectory() as tmp:
            lv2, docs = os.path.join(tmp, "lv2"), os.path.join(tmp, "doc")
            for bundle in ("calf.lv2", "blob.lv2", "orphan.lv2"):
                os.makedirs(os.path.join(lv2, bundle))
            with open(os.path.join(lv2, "calf.lv2", "manifest.ttl"), "w", encoding="utf-8") as f:
                f.write("<http://calf.sourceforge.net/plugins/Reverb>\n    a lv2:Plugin ;\n    rdfs:seeAlso <reverb.ttl> .\n")
            for package, name in (("calf-plugins", "copyright-gpl2plus"), ("blob-plugins", "copyright-proprietary")):
                os.makedirs(os.path.join(docs, package))
                with open(os.path.join(docs, package, "copyright"), "w", encoding="utf-8") as f:
                    f.write(fixture(name))
            owner = {"calf.lv2": "calf-plugins", "blob.lv2": "blob-plugins"}
            result = pl.collect(lv2, docs, lambda path: owner.get(os.path.basename(path)), lambda package: None)
            self.assertEqual(result["calf.lv2"], {"license": "LGPL-2.1-or-later", "package": "calf-plugins",
                                                  "source_url": "https://github.com/calf-studio-gear/calf"})
            self.assertEqual(result["lv2:http://calf.sourceforge.net/plugins/Reverb"], result["calf.lv2"])
            self.assertNotIn("blob.lv2", result)        # unrecognised licence: no entry
            self.assertNotIn("orphan.lv2", result)      # no owning package: no entry

    def _avldrums(self, soundfont_fixture):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        lv2, docs = os.path.join(tmp.name, "lv2"), os.path.join(tmp.name, "doc")
        os.makedirs(os.path.join(lv2, "avldrums.lv2"))
        for package, name in (("avldrums.lv2", "copyright-gpl2plus"), ("avldrums.lv2-soundfont", soundfont_fixture)):
            os.makedirs(os.path.join(docs, package))
            with open(os.path.join(docs, package, "copyright"), "w", encoding="utf-8") as f:
                f.write(fixture(name))
        return pl.collect(lv2, docs, lambda path: "avldrums.lv2", lambda package: None)

    def test_avldrums_combines_plugin_and_soundfont_licences(self):
        result = self._avldrums("copyright-spdx")
        self.assertEqual(result["avldrums.lv2"]["license"], "LGPL-2.1-or-later AND LGPL-3.0-or-later")
        result = self._avldrums("copyright-mixed")
        self.assertEqual(result["avldrums.lv2"]["license"], "LGPL-2.1-or-later AND (GPL-2.0-or-later OR MIT)")

    def test_avldrums_with_unknown_soundfont_licence_has_no_entry(self):
        self.assertEqual(self._avldrums("copyright-proprietary"), {})


if __name__ == "__main__":
    unittest.main()
