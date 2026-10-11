#!/usr/bin/env bash
#
# One-time setup for the benchmarks: builds the TSBS tools, pulls the TimescaleDB image and
# generates the dataset. Everything goes into the work directory, which must be on a native
# Linux filesystem (not a Windows drive mounted into WSL).
#
# usage: setup.sh

set -euo pipefail

source "$(dirname "$0")/config.sh"

mkdir -p "${WORK}/bin" "${WORK}/data" "${WORK}/results"

if [[ ! -x "${WORK}/bin/tsbs_load_timescaledb" ]]; then
  echo "== building TSBS"
  if [[ ! -d "${WORK}/tsbs" ]]; then
    git clone https://github.com/timescale/tsbs.git "${WORK}/tsbs"
  fi
  git -C "${WORK}/tsbs" checkout -q "${TSBS_COMMIT}"
  # The one change made to TSBS. Its TimescaleDB loader passes `replication_factor => NULL`
  # to create_hypertable, an argument that TimescaleDB 2.14 removed along with multi-node.
  # Swapping it for an argument that changes nothing lets the loader work with a current
  # TimescaleDB instead of pinning the baseline to a 2023 release.
  sed -i 's/"replication_factor => NULL"/"if_not_exists => FALSE"/' \
    "${WORK}/tsbs/pkg/targets/timescaledb/creator.go"
  # TSBS's go.sum lacks entries for some indirect dependencies; -mod=mod lets Go add them.
  (cd "${WORK}/tsbs" && GOFLAGS=-mod=mod go build -o "${WORK}/bin/" \
    ./cmd/tsbs_generate_data ./cmd/tsbs_load_influx ./cmd/tsbs_load_timescaledb \
    ./cmd/tsbs_generate_queries ./cmd/tsbs_run_queries_timescaledb)
fi

echo "== pulling ${TIMESCALE_IMAGE}"
docker pull -q "${TIMESCALE_IMAGE}"

if [[ ! -s "${DATA_INFLUX}" ]]; then
  echo "== generating ${SCALE} hosts, ${TS_START} to ${TS_END}, every ${LOG_INTERVAL}"
  for format in influx timescaledb; do
    "${WORK}/bin/tsbs_generate_data" --use-case=cpu-only --seed="${SEED}" --scale="${SCALE}" \
      --timestamp-start="${TS_START}" --timestamp-end="${TS_END}" \
      --log-interval="${LOG_INTERVAL}" --format="${format}" > "${WORK}/data/cpu.${format}"
  done
fi

echo "== dataset"
ls -l "${WORK}/data"
echo "rows:   $(wc -l < "${DATA_INFLUX}")"
echo "points: $(($(wc -l < "${DATA_INFLUX}") * 10))"
