#!/usr/bin/env bash
#
# The whole benchmark, start to finish. Run setup.sh and build.sh first.
#
#   1. Ingest: each system is loaded from scratch LOAD_RUNS times.
#   2. Storage: size on disk after the last load, for TimescaleDB before and after compression.
#   3. Queries: the same queries against both systems, answers compared, then timed. Done
#      against TimescaleDB both uncompressed and compressed.
#   4. A cross-check of this harness: the official TSBS query runner against TimescaleDB.
#   5. Block size: tsengine reloaded with smaller blocks, to measure aggregation push-down.
#
# Everything is written to ${WORK}/results. Takes about 15 minutes.
#
# usage: run_all.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
source "${HERE}/lib.sh"

LOAD_RUNS="${LOAD_RUNS:-5}"
RESULTS="${WORK}/results"
rm -rf "${RESULTS}"
mkdir -p "${RESULTS}"

section() {
  echo
  echo "=================== $*"
}

section "environment"
{
  echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "cpu=$(lscpu | sed -n 's/^Model name: *//p')"
  echo "cpus=$(nproc)"
  echo "memory_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)"
  echo "kernel=$(uname -r)"
  echo "os=$(. /etc/os-release && echo "${PRETTY_NAME}")"
  echo "filesystem=$(df -T "${WORK}" | awk 'NR == 2 {print $2}')"
  echo "docker=$(docker version --format '{{.Server.Version}}')"
  echo "timescale_image=${TIMESCALE_IMAGE}"
  echo "tsbs_commit=$(git -C "${WORK}/tsbs" rev-parse HEAD)"
  echo "tsengine_commit=$(git -C "${HERE}/.." rev-parse HEAD)"
  echo "tsengine_dirty=$(git -C "${HERE}/.." status --porcelain | wc -l)"
  echo "compiler=$(c++ --version | head -1)"
  echo "go=$(go version)"
  echo "scale=${SCALE}"
  echo "ts_start=${TS_START}"
  echo "ts_end=${TS_END}"
  echo "log_interval=${LOG_INTERVAL}"
  echo "load_workers=${LOAD_WORKERS}"
  echo "load_batch_size=${LOAD_BATCH_SIZE}"
  echo "load_runs=${LOAD_RUNS}"
} | tee "${RESULTS}/environment.txt"

section "ingest: tsengine x ${LOAD_RUNS}"
for run in $(seq 1 "${LOAD_RUNS}"); do
  pkill -x tsengine_server 2>/dev/null || true
  bash "${HERE}/load.sh" tsengine "run${run}" | grep -E 'metrics_per_sec|bytes_blocks'
done
pkill -x tsengine_server 2>/dev/null || true

section "ingest: timescaledb x ${LOAD_RUNS}"
for run in $(seq 1 "${LOAD_RUNS}"); do
  bash "${HERE}/load.sh" timescaledb "run${run}" | grep -E 'metrics_per_sec|bytes_uncompressed'
done

section "queries: timescaledb uncompressed"
bash "${HERE}/queries.sh" uncompressed

section "compress timescaledb"
bash "${HERE}/compress.sh" final

section "queries: timescaledb compressed"
bash "${HERE}/queries.sh" compressed

section "official TSBS query runner against timescaledb (compressed)"
for type in single-groupby-1-1-1 double-groupby-1; do
  "${WORK}/bin/tsbs_generate_queries" --use-case=cpu-only --seed="${SEED}" --scale="${SCALE}" \
    --timestamp-start="${TS_START}" --timestamp-end="${TS_END}" \
    --queries=1000 --query-type="${type}" --format=timescaledb \
    > "${WORK}/data/queries-${type}.gob" 2> /dev/null
  "${WORK}/bin/tsbs_run_queries_timescaledb" --file="${WORK}/data/queries-${type}.gob" \
    --hosts=127.0.0.1 --port="${TIMESCALE_PORT}" --user=postgres \
    --pass="${TIMESCALE_PASSWORD}" --db-name=benchmark --workers=1 --print-interval=0 \
    --postgres="sslmode=disable" > "${RESULTS}/tsbs-official-${type}.txt" 2>&1
  grep -E 'min:|med:' "${RESULTS}/tsbs-official-${type}.txt" | head -2
done
docker stats --no-stream --format 'container_memory_after_queries={{.MemUsage}}' \
  "${TIMESCALE_CONTAINER}" | tee -a "${RESULTS}/environment.txt"

section "block size: tsengine with smaller blocks"
for points in 360 128; do
  pkill -x tsengine_server 2>/dev/null || true
  bash "${HERE}/load.sh" tsengine "blocks${points}" --block-points "${points}" |
    grep -E 'metrics_per_sec|bytes_blocks'
  pkill -x tsengine_server 2>/dev/null || true
  bash "${HERE}/queries.sh" "blocks${points}" --block-points "${points}"
done
timescale_stop

section "done"
ls "${RESULTS}"
