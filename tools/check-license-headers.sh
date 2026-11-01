#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Fails if any first-party C++ source lacks the AGPL SPDX + copyright header.
set -euo pipefail

cd "$(dirname "$0")/.."

status=0
while IFS= read -r -d '' f; do
    if [ "$(sed -n 1p "$f")" != "// SPDX-License-Identifier: AGPL-3.0-or-later" ] \
       || [ "$(sed -n 2p "$f")" != "// Copyright (C) 2026 Rovox contributors" ]; then
        echo "missing license header: $f" >&2
        status=1
    fi
done < <(find src tests spikes \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.mm' \) -print0 2>/dev/null)

exit $status
