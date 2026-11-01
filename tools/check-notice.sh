#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Fails if a git submodule listed in .gitmodules is not mentioned (by path) in NOTICE.md.
set -euo pipefail

cd "$(dirname "$0")/.."

status=0
while read -r _ path; do
    if ! grep -qF "$path" NOTICE.md; then
        echo "NOTICE.md does not mention submodule: $path" >&2
        status=1
    fi
done < <(git config -f .gitmodules --get-regexp '\.path$')

exit $status
