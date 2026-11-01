#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Turn the JSON files written by run_matrix.sh into one markdown table."""
import glob
import json
import os
import re
import sys

d = sys.argv[1]
rows = []
for exit_file in sorted(glob.glob(os.path.join(d, "*.exit"))):
    name = os.path.basename(exit_file)[:-5]
    m = re.match(r"(\w+?)_cpus(\d+)_mem(\w+)", name)
    scen, cpus, mem = m.groups()
    code = int(open(exit_file).read().strip() or -1)
    jf = os.path.join(d, name + ".json")
    if code != 0 or not os.path.exists(jf):
        rows.append((scen, cpus, mem, "OOM-killed" if code == 137 else f"exit {code}", "", "", "", "", "", "", ""))
        continue
    j = json.load(open(jf))
    r = j["results"][0]
    off, rt = r["offline"], r["realtime"]
    rows.append((scen, cpus, mem, "ok", f'{off["speed_factor"]:.1f}x',
                 f'{rt["cpu_load_p50_percent_of_one_core"]:.0f}%', f'{rt["block_time_us_p99"]:.0f}',
                 f'{rt["underrun_per_min"]:.1f}', f'{r["project_open_ms"]:.0f}', f'{r["rss_now_mb"]:.0f}', f'{r["cgroup_mem_peak_mb"]:.0f}'))

print("| scenario | cpus | mem | status | offline speed | RT load p50 (1 core) | block p99 us | underruns/min | open ms | RSS after run MB | cgroup peak MB |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for row in rows:
    print("| " + " | ".join(row) + " |")
