#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Entry point of the Playwright runner (docker/compose.e2e.yml): installs the npm dependencies and gives the container an audio device. Firefox
# never resolves AudioContext.resume() without one (Chromium has a fake device), so the stream tests need a PulseAudio null sink.
set -eu
npm ci --no-audit --no-fund       # the lockfile pins the e2e dependencies
if ! command -v pulseaudio >/dev/null 2>&1; then
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends pulseaudio >/dev/null
fi
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p "$XDG_RUNTIME_DIR"
pulseaudio --start --exit-idle-time=-1 --load="module-null-sink" 2>/dev/null
exec "$@"
