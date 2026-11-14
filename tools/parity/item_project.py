# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Generated project for fixtures with "items": one track, items that play one source file with their own play rate (preserve pitch on or
# off) and source offset, no FX. Used for the stretch-tone fixtures: the item flags decide whether REAPER's stretcher colours the sound.
# Item: {"pos": s, "len": s, "soffs": s, "rate": 1.1, "preserve": 1}. REAPER renders the project in a new tab (reaper_driver.run_project).


def write_rpp(path, signal_wav, items, sample_rate=48000):
    lines = ['<REAPER_PROJECT 0.1 "7.81/macOS-arm64" 0', "  SAMPLERATE %d 1 0" % sample_rate, "  <TRACK", "    NAME items", "    NCHAN 2"]
    for it in items:
        lines += ["    <ITEM", "      POSITION %.9f" % it.get("pos", 0.0), "      LENGTH %.9f" % it["len"], "      SOFFS %.9f" % it.get("soffs", 0.0),
                  "      PLAYRATE %.12g %d 0 -1 0 0.0025" % (it.get("rate", 1.0), 1 if it.get("preserve", 1) else 0),
                  "      <SOURCE WAVE", '        FILE "%s"' % signal_wav, "      >", "    >"]
    lines += ["  >", ">", ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return path


def length_s(items):
    return max(it.get("pos", 0.0) + it["len"] for it in items)
