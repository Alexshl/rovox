#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Fails if a first-party source file lacks the AGPL SPDX + copyright header.
#  - C/C++/ObjC++ (src tests spikes): the first two lines must be the `//` header, exactly.
#  - Scripts, CMake, Docker, YAML, HTML, TypeScript, CSS (everything else we write, web/ included): the header must appear in the first three
#    lines (a shebang or `# syntax=` line may come first), in whatever comment syntax the file uses.
set -euo pipefail

cd "$(dirname "$0")/.."

spdx="SPDX-License-Identifier: AGPL-3.0-or-later"
copyright="Copyright (C) 2026 Rovox contributors"
status=0

while IFS= read -r -d '' f; do
    if [ "$(sed -n 1p "$f")" != "// $spdx" ] || [ "$(sed -n 2p "$f")" != "// $copyright" ]; then
        echo "missing license header: $f" >&2
        status=1
    fi
done < <(find src tests spikes \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.mm' \) -print0 2>/dev/null)

while IFS= read -r -d '' f; do
    head3="$(head -n 3 "$f")"
    case "$head3" in
        *"$spdx"*"$copyright"*) ;;
        *) echo "missing license header: $f" >&2; status=1 ;;
    esac
done < <(find src tests spikes tools cmake docker .github web \
              \( -name node_modules -o -name dist \) -prune -o \
              \( -name '*.py' -o -name '*.sh' -o -name '*.cmake' -o -name 'CMakeLists.txt' -o -name 'Dockerfile' \
                 -o -name '*.yml' -o -name '*.yaml' -o -name '*.html' -o -name '*.js' -o -name '*.mjs' -o -name '*.ts' -o -name '*.css' \) -print0 2>/dev/null
         printf '%s\0' CMakeLists.txt)

exit $status
