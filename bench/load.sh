#!/usr/bin/env bash
#
# Loads the dataset into one system from scratch and records how long it took and how much
# disk the data uses. Both systems are loaded by the stock TSBS loader for their format,
# with the same number of workers and the same batch size.
#
# usage: load.sh tsengine|timescaledb <label> [tsengine server flags...]
#
# Writes ${WORK}/results/load-<system>-<label>.txt (key=value lines) and leaves the system
# running with the data loaded, ready for the query benchmark.

set -euo pipefail

source "$(dirname "$0")/lib.sh"

SYSTEM="$1"
LABEL="$2"
shift 2

OUT="${WORK}/results/load-${SYSTEM}-${LABEL}.txt"
LOG="${WORK}/results/load-${SYSTEM}-${LABEL}.log"
ROWS="$(wc -l < "${DATA_INFLUX}")"
POINTS=$((ROWS * 10))
: > "${OUT}"

record() {
  echo "$1=$2" | tee -a "${OUT}"
}

record system "${SYSTEM}"
record label "${LABEL}"
record rows "${ROWS}"
record points "${POINTS}"
record workers "${LOAD_WORKERS}"
record batch_size "${LOAD_BATCH_SIZE}"

# The loader prints "loaded N metrics in Xsec with W workers (mean rate R metrics/sec)".
loader_rate() {
  grep -E 'loaded [0-9]+ metrics' "${LOG}" | sed -E 's/.*mean rate ([0-9.]+) metrics\/sec.*/\1/'
}
loader_seconds() {
  grep -E 'loaded [0-9]+ metrics' "${LOG}" | sed -E 's/.*metrics in ([0-9.]+)sec.*/\1/'
}

case "${SYSTEM}" in
  tsengine)
    DIR="${WORK}/tsengine-data"
    rm -rf "${DIR}"
    record server_flags "$*"
    tsengine_start "${DIR}" "$@"
    "${WORK}/bin/tsbs_load_influx" --file="${DATA_INFLUX}" --urls="${TSENGINE_URL}" \
      --workers="${LOAD_WORKERS}" --batch-size="${LOAD_BATCH_SIZE}" --gzip=false \
      > "${LOG}" 2>&1
    record load_seconds "$(loader_seconds)"
    record metrics_per_sec "$(loader_rate)"
    record peak_rss_kb_after_load "$(tsengine_peak_rss_kb)"
    record bytes_after_load "$(du -sb "${DIR}" | cut -f1)"

    # Settle the data the way a database would be left for querying: everything flushed and
    # each partition merged into one file.
    start_ns="$(date +%s%N)"
    curl -sf -X POST "${TSENGINE_URL}/api/v1/flush" > /dev/null
    curl -sf -X POST "${TSENGINE_URL}/api/v1/compact" > /dev/null
    record compact_seconds "$(awk "BEGIN {print ($(date +%s%N) - ${start_ns}) / 1e9}")"
    record bytes_total "$(du -sb "${DIR}" | cut -f1)"
    record bytes_blocks "$(du -sb "${DIR}/blocks" | cut -f1)"
    record bytes_series_log "$(stat -c %s "${DIR}/series.log")"
    record block_files "$(find "${DIR}/blocks" -name '*.tsb' | wc -l)"
    record stats "$(curl -sf "${TSENGINE_URL}/api/v1/stats")"
    count="$(curl -sf "${TSENGINE_URL}/api/v1/query?measurement=cpu&agg=count" |
      jq '.results[0].values[0]')"
    record points_stored "${count}"
    ;;

  timescaledb)
    timescale_start_fresh
    record version "$(timescale_sql postgres "SELECT version()" | cut -d' ' -f1-2)"
    record shared_buffers "$(timescale_sql postgres "SHOW shared_buffers")"
    "${WORK}/bin/tsbs_load_timescaledb" --file="${DATA_TIMESCALE}" \
      --host=127.0.0.1 --port="${TIMESCALE_PORT}" --user=postgres \
      --pass="${TIMESCALE_PASSWORD}" --db-name=benchmark \
      --workers="${LOAD_WORKERS}" --batch-size="${LOAD_BATCH_SIZE}" \
      > "${LOG}" 2>&1
    record load_seconds "$(loader_seconds)"
    record metrics_per_sec "$(loader_rate)"
    record extension "$(timescale_sql benchmark \
      "SELECT extversion FROM pg_extension WHERE extname = 'timescaledb'")"
    record chunks "$(timescale_sql benchmark "SELECT count(*) FROM show_chunks('cpu')")"
    record bytes_uncompressed "$(timescale_sql benchmark "SELECT hypertable_size('cpu')")"

    record bytes_tags "$(timescale_sql benchmark "SELECT pg_total_relation_size('tags')")"
    record rows_stored "$(timescale_sql benchmark "SELECT count(*) FROM cpu")"
    record container_memory "$(docker stats --no-stream --format '{{.MemUsage}}' \
      "${TIMESCALE_CONTAINER}")"
    # Planner statistics, so queries against the uncompressed table are planned properly.
    timescale_sql benchmark "VACUUM ANALYZE" > /dev/null
    ;;

  *)
    echo "usage: load.sh tsengine|timescaledb <label> [server flags...]" >&2
    exit 2
    ;;
esac
