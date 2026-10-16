#!/usr/bin/env bash
# Game Compressor TV app - stages the payload for the package, as
# gc/game-compressor.elf with its version in gc/version.txt (read by
# src/gc/starter.cpp). Adapted from the Orbit Store TV app (GPL-3.0-or-later).
# Usage: stage-payload.sh <game-compressor.elf | none> <stage directory> [version]
set -euo pipefail
payload=$1
stage=$2
version=${3:-unknown}
rm -rf -- "$stage"
mkdir -p "$stage"
if [[ $payload == none ]]; then
    printf '%s\n' '==> [payload] Not bundling Game Compressor (GC_PAYLOAD=none)'
    exit 0
fi
if [[ ! -f $payload ]]; then
    printf 'Game Compressor payload not found: %s\n' "$payload" >&2
    printf '%s\n' 'Build it first (make in the repository root), or pass GC_PAYLOAD=none.' >&2
    exit 1
fi
size=$(stat -c %s "$payload")
if [[ $(head -c 4 "$payload" | od -An -tx1 | tr -d ' \n') != 7f454c46 ]] || ((size > 64 * 1024 * 1024)); then
    printf 'Not a payload (ELF, at most 64 MiB): %s\n' "$payload" >&2
    exit 1
fi
mkdir -p "$stage/gc"
cp "$payload" "$stage/gc/game-compressor.elf"
printf '%s\n' "$version" > "$stage/gc/version.txt"
printf '==> [payload] Bundling Game Compressor %s (%s bytes)\n' "$version" "$size"
