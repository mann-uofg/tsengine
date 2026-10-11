#!/usr/bin/env bash
#
# Builds tsengine outside the source tree, in the benchmark work directory.
#
# usage: build.sh [release|debug]

set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${TSBENCH_WORK:-$HOME/tsbench}"
KIND="${1:-release}"

case "${KIND}" in
  release) FLAGS=(-DCMAKE_BUILD_TYPE=Release -DTSENGINE_SANITIZE=OFF) ;;
  debug) FLAGS=(-DCMAKE_BUILD_TYPE=Debug -DTSENGINE_SANITIZE=ON) ;;
  *) echo "usage: build.sh [release|debug]" >&2; exit 2 ;;
esac

mkdir -p "${WORK}"
cmake -S "${SRC}" -B "${WORK}/build-${KIND}" -G Ninja "${FLAGS[@]}"
cmake --build "${WORK}/build-${KIND}"
echo "built ${WORK}/build-${KIND}"
