#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Resource benchmark matrix (task 080): runs `rovox_spikes bench` in containers with --cpus / --memory limits and
# writes one JSON per run plus a markdown summary.
#
#   tools/bench/run_matrix.sh [image] [out-dir]
#   env: CPUS="1 2 4"  MEMS="1g 2g 4g"  SCENARIOS="s1 s2 s3 s4"  SONG_SECONDS=240  RT_SECONDS=20  EXTRA_ARGS=""
#        PLATFORM=linux/amd64 (to run a foreign-arch image under QEMU; numbers are then meaningless)
#        BENCH_BIN=rovox_spikes  binary inside the image;  DOCKER_ARGS=""  extra `docker run` args (e.g. a bind mount)
#
# Build the image first:  tools/build-in-docker.sh spikes rovox-spikes --build-arg ROVOX_BUILD_SPIKES=ON
set -uo pipefail

cd "$(dirname "$0")/../.."

image="${1:-rovox-spikes}"
out="${2:-build/bench}"
cpus="${CPUS:-1 2 4}"
mems="${MEMS:-1g 2g 4g}"
scenarios="${SCENARIOS:-s1 s2 s3 s4}"
song="${SONG_SECONDS:-240}"
rt="${RT_SECONDS:-20}"
mkdir -p "$out"
out_abs="$(cd "$out" && pwd)"

bin="${BENCH_BIN:-rovox_spikes}"
extra_docker=(${DOCKER_ARGS:-})
platform_args=()
if [ -n "${PLATFORM:-}" ]; then platform_args=(--platform "$PLATFORM"); fi

for s in $scenarios; do
  for c in $cpus; do
    for m in $mems; do
      name="${s}_cpus${c}_mem${m}"
      echo "== $name"
      docker run --rm ${platform_args[@]+"${platform_args[@]}"} ${extra_docker[@]+"${extra_docker[@]}"} --network none --cpus="$c" --memory="$m" --memory-swap="$m" \
          -v "$out_abs":/out --entrypoint "$bin" "$image" \
          bench /tmp/bench --scenario "$s" --seconds "$song" --rt-seconds "$rt" --json "/out/$name.json" ${EXTRA_ARGS:-} \
          > "$out_abs/$name.log" 2>&1
      code=$?
      echo "$code" > "$out_abs/$name.exit"
      if [ "$code" = 137 ]; then echo "   OOM-killed (exit 137)"; fi
    done
  done
done

python3 "$(dirname "$0")/summarize.py" "$out_abs" > "$out_abs/summary.md"
cat "$out_abs/summary.md"
