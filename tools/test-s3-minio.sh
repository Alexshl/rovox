#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# Object storage as the source of truth, against a real S3 server (MinIO) in Docker: save -> kill -9 -> open in a fresh container with an EMPTY data volume.
#   tools/test-s3-minio.sh [image]            (default image: rovox)
# Environment: S3TEST_RPP=/path/on/host/project.RPP  S3TEST_MOUNT=/dir/on/host  (optional: import that REAPER project instead of generated WAVs;
#   the directory is mounted read-only at /in and, for absolute media paths, at its own path; S3TEST_JSFX=/dir with REAPER Effects mounted at /jsfx)
# Uses its own network, containers (rovox-s3t-*) and volumes; touches nothing else; cleans up on exit.
set -euo pipefail
cd "$(dirname "$0")/.."

image="${1:-rovox}"
net=rovox-s3t-net
token=s3-test-token
bucket=rovox-test
ttl=10
tmp="$(mktemp -d)"
trap 'cleanup' EXIT

cleanup() {
    docker rm -f rovox-s3t-minio rovox-s3t-a rovox-s3t-b >/dev/null 2>&1 || true
    docker volume rm -f rovox-s3t-data-a rovox-s3t-data-b >/dev/null 2>&1 || true
    docker network rm "$net" >/dev/null 2>&1 || true
    rm -rf "$tmp"
}

docker network create "$net" >/dev/null
docker run -d --name rovox-s3t-minio --network "$net" -p 127.0.0.1:19000:9000 -e MINIO_ROOT_USER=minioadmin -e MINIO_ROOT_PASSWORD=minioadmin-secret minio/minio server /data >/dev/null
# the bucket is created and inspected with curl's built-in SigV4 (no mc image needed)
s3curl() { curl -fsS --aws-sigv4 "aws:amz:us-east-1:s3" --user minioadmin:minioadmin-secret "$@"; }
keys() { s3curl "http://127.0.0.1:19000/$bucket?list-type=2" | grep -o '<Key>[^<]*' | sed 's/<Key>//'; }
for i in $(seq 1 30); do
    s3curl -X PUT "http://127.0.0.1:19000/$bucket" >/dev/null 2>&1 && break
    sleep 1
done

mounts=()
seed_args=(--fx)
if [ -n "${S3TEST_RPP:-}" ]; then
    mounts+=(-v "$S3TEST_MOUNT:/in:ro" -v "$S3TEST_MOUNT:$S3TEST_MOUNT:ro")
    [ -n "${S3TEST_JSFX:-}" ] && mounts+=(-v "$S3TEST_JSFX:/jsfx:ro")
    seed_args+=(--rpp "/in/${S3TEST_RPP#"$S3TEST_MOUNT"/}")
    allow=(--allow-path /in --allow-path "$S3TEST_MOUNT")
else
    python3 - "$tmp" <<'PY'
import math, struct, sys, wave
for n, hz in enumerate((220, 330, 440), 1):
    with wave.open(f"{sys.argv[1]}/take{n}.wav", "w") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
        w.writeframes(b"".join(struct.pack("<h", int(9000 * math.sin(2 * math.pi * hz * i / 44100))) for i in range(44100 * 2)))
PY
    mounts+=(-v "$tmp:/in:ro")
    seed_args+=(--wav /in/take1.wav /in/take2.wav /in/take3.wav)
    allow=(--allow-path /in)
fi

s3env=(-e S3_ENDPOINT=http://rovox-s3t-minio:9000 -e S3_BUCKET="$bucket" -e S3_ACCESS_KEY=minioadmin -e S3_SECRET_KEY=minioadmin-secret -e S3_REGION=us-east-1
       -e S3_PREFIX=rovox/ -e ROVOX_LEASE_TTL_S="$ttl" -e ROVOX_TOKEN="$token")

start() {   # name volume host-port
    docker run -d --name "$1" --network "$net" -v "$2:/data" -p "127.0.0.1:$3:4810" "${s3env[@]}" "${mounts[@]}" "$image" serve --bind 0.0.0.0 --storage s3 "${allow[@]}" >/dev/null
    for i in $(seq 1 60); do
        curl -fsS -H "Authorization: Bearer $token" "http://127.0.0.1:$3/api/projects" >/dev/null 2>&1 && return 0
        sleep 1
    done
    docker logs "$1" >&2
    echo "server $1 did not start" >&2
    return 1
}

echo "== server A (empty data volume)"
start rovox-s3t-a rovox-s3t-data-a 14811
python3 tools/s3_e2e.py seed --http http://127.0.0.1:14811/mcp --token "$token" --state "$tmp/state.json" "${seed_args[@]}"

echo "== kill -9 server A (no graceful shutdown, no lease release)"
docker kill rovox-s3t-a >/dev/null
echo "== bucket content"
keys | sed 's/^/   /' | head -40

echo "== server B (NEW container, EMPTY data volume)"
start rovox-s3t-b rovox-s3t-data-b 14812
python3 tools/s3_e2e.py verify --http http://127.0.0.1:14812/mcp --token "$token" --state "$tmp/state.json" --wait-lease $((ttl * 4))
echo "== renders in the bucket"
keys | grep renders/ | sed 's/^/   /'
keys | grep -q 'renders/s3check' || { echo "no render object in the bucket" >&2; exit 1; }

echo "== SIGTERM flushes and releases the lease"
docker stop -t 40 rovox-s3t-b >/dev/null
if keys | grep -q LEASE; then echo "a LEASE object is left after a clean shutdown" >&2; exit 1; fi
echo "S3 MINIO TEST OK"
