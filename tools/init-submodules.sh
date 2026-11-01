#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Fetch submodules for anonymous clones / CI.
#
# Tracktion Engine declares its nested JUCE submodule with an ssh URL (git@github.com:juce-framework/JUCE.git),
# which fails without an ssh key. We rewrite it to https for this one command only (git -c is inherited by the
# recursive child git processes), so nothing is written to your git config and no fork is needed.
#
# Usage: tools/init-submodules.sh [--full]     (default: --depth 1; GitHub allows fetching the pinned SHA)
set -euo pipefail

cd "$(dirname "$0")/.."

depth=(--depth 1)
if [ "${1:-}" = "--full" ]; then
    depth=()
fi

exec git -c url."https://github.com/".insteadOf="git@github.com:" \
    submodule update --init --recursive "${depth[@]}"
