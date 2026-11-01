#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Build a Docker image target. Usage: tools/build-in-docker.sh [target] [tag] [extra docker args...]
# Defaults: target=runtime, tag=rovox. Set PLATFORM=linux/amd64 to cross-build via QEMU.
set -euo pipefail

cd "$(dirname "$0")/.."

target="${1:-runtime}"
tag="${2:-rovox}"
shift $(( $# > 2 ? 2 : $# )) || true

sha="$(git describe --always --dirty 2>/dev/null || echo unknown)"

args=(--file docker/Dockerfile --target "$target" --tag "$tag" --build-arg "ROVOX_GIT_SHA=$sha")
if [ -n "${ROVOX_BUILD_JOBS:-}" ]; then
    args+=(--build-arg "ROVOX_BUILD_JOBS=$ROVOX_BUILD_JOBS")
fi
if [ -n "${PLATFORM:-}" ]; then
    args+=(--platform "$PLATFORM")
fi

exec docker build "${args[@]}" "$@" .
