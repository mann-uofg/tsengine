#!/usr/bin/env bash
#
# Runs the query benchmark against whatever is currently loaded: tsengine from its data
# directory, and TimescaleDB if its container exists. Every query is first checked to give
# the same answer on both systems, then timed.
#
# usage: queries.sh <label> [tsengine server flags...] [-- queryrunner flags...]

set -euo pipefail

source "$(dirname "$0")/lib.sh"

LABEL="$1"
shift
SERVER_FLAGS=()
while [[ $# -gt 0 && "$1" != "--" ]]; do
  SERVER_FLAGS+=("$1")
  shift
done
if [[ $# -gt 0 ]]; then
  shift
fi

(cd "$(dirname "$0")/queryrunner" && go build -o "${WORK}/bin/queryrunner" .)

PG=""
if [[ "${SKIP_TIMESCALE:-0}" != "1" ]] && timescale_resume; then
  PG="postgres://postgres:${TIMESCALE_PASSWORD}@127.0.0.1:${TIMESCALE_PORT}/benchmark"
  PG="${PG}?sslmode=disable"
fi

trap tsengine_stop EXIT
tsengine_start "${WORK}/tsengine-data" "${SERVER_FLAGS[@]}"

"${WORK}/bin/queryrunner" --tsengine="${TSENGINE_URL}" --timescaledb="${PG}" \
  --hosts="${SCALE}" --start="${TS_START}" --end="${TS_END}" --label="${LABEL}" \
  --out="${WORK}/results/queries-${LABEL}.json" "$@"
echo "wrote ${WORK}/results/queries-${LABEL}.json"
