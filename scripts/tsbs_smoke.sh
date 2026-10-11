#!/usr/bin/env bash
#
# End-to-end check with real TSBS tools.
#
# Generates a small TSBS cpu-only dataset, loads it into a running tsengine_server with the
# stock TSBS InfluxDB loader, and checks through the query API that the engine holds exactly
# what was generated. Then kills the server with SIGKILL, starts it again and repeats the
# checks, so recovery is exercised on real data too.
#
# usage: tsbs_smoke.sh <path to tsengine_server> <dir with tsbs_generate_data, tsbs_load_influx>

set -euo pipefail

SERVER="$1"
TSBS_BIN="$2"

HOSTS=10
PORT=18086
URL="http://127.0.0.1:${PORT}"
WORK="$(mktemp -d)"
DATA="${WORK}/cpu.lp"
SERVER_PID=""

stop_server() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill -9 "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=""
  fi
}
trap stop_server EXIT

start_server() {
  # A small flush threshold, so the data ends up spread over the WAL, the memtable and
  # several block files, and compaction runs.
  "${SERVER}" --dir "${WORK}/db" --port "${PORT}" --flush-points 50000 \
    >> "${WORK}/server.log" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 100); do
    if curl -sf "${URL}/health" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "server did not start:" >&2
  cat "${WORK}/server.log" >&2
  return 1
}

# query <params> <jq filter>: runs a query and prints one value from the JSON answer.
query() {
  curl -sf "${URL}/api/v1/query?$1" | jq "$2"
}

expect() {
  local what="$1" want="$2" got="$3"
  if [[ "${want}" != "${got}" ]]; then
    echo "FAIL: ${what}: want ${want}, got ${got}" >&2
    exit 1
  fi
  echo "ok: ${what} = ${got}"
}

check_contents() {
  local lines="$1"
  expect "points in all cpu series" "$((lines * 10))" \
    "$(query 'measurement=cpu&agg=count' '.results[0].values[0]')"
  expect "points in usage_user" "${lines}" \
    "$(query 'measurement=cpu&field=usage_user&agg=count' '.results[0].values[0]')"
  expect "cpu series" "$((HOSTS * 10))" \
    "$(query 'measurement=cpu&agg=count' '.stats.series')"
  expect "hosts when grouped by hostname" "${HOSTS}" \
    "$(query 'measurement=cpu&field=usage_user&agg=max&group_by=hostname' '.results | length')"
  expect "points of one host" "$((lines / HOSTS))" \
    "$(query 'measurement=cpu&field=usage_user&agg=count&tag=hostname:host_3' '.results[0].values[0]')"
}

echo "== generating data"
"${TSBS_BIN}/tsbs_generate_data" --use-case=cpu-only --seed=123 --scale="${HOSTS}" \
  --timestamp-start=2016-01-01T00:00:00Z --timestamp-end=2016-01-01T06:00:00Z \
  --log-interval=10s --format=influx > "${DATA}"
LINES="$(wc -l < "${DATA}")"
echo "generated ${LINES} lines"
if [[ "${LINES}" -lt 1000 ]]; then
  echo "FAIL: suspiciously little data" >&2
  exit 1
fi

echo "== loading with tsbs_load_influx"
start_server
"${TSBS_BIN}/tsbs_load_influx" --file="${DATA}" --urls="${URL}" --workers=2 --batch-size=1000 \
  --gzip=false

echo "== checking what was loaded"
check_contents "${LINES}"
curl -sf "${URL}/api/v1/stats"
echo

echo "== killing the server and recovering"
stop_server
start_server
check_contents "${LINES}"
curl -sf "${URL}/api/v1/stats"
echo

echo "PASS"
