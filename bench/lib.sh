#!/usr/bin/env bash
#
# Helpers to start and stop the two systems. Sourced, not run.

source "$(dirname "${BASH_SOURCE[0]}")/config.sh"

TSENGINE_PID=""

# tsengine_start <data dir> [server flags...]
tsengine_start() {
  local dir="$1"
  shift
  "${TSENGINE_SERVER}" --dir "${dir}" --port "${TSENGINE_PORT}" "$@" \
    >> "${WORK}/results/tsengine-server.log" 2>&1 &
  TSENGINE_PID=$!
  # Startup replays the WAL, so allow it some time.
  for _ in $(seq 1 600); do
    if curl -sf "${TSENGINE_URL}/health" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "tsengine did not start; see ${WORK}/results/tsengine-server.log" >&2
  return 1
}

tsengine_stop() {
  if [[ -n "${TSENGINE_PID}" ]]; then
    kill -TERM "${TSENGINE_PID}" 2>/dev/null || true
    wait "${TSENGINE_PID}" 2>/dev/null || true
    TSENGINE_PID=""
  fi
}

# Peak resident memory of the running server, in kB.
tsengine_peak_rss_kb() {
  awk '/VmHWM/ {print $2}' "/proc/${TSENGINE_PID}/status"
}

# timescale_sql <database> <sql>: runs one statement and prints the result unformatted.
timescale_sql() {
  docker exec -e PGPASSWORD="${TIMESCALE_PASSWORD}" "${TIMESCALE_CONTAINER}" \
    psql -h 127.0.0.1 -U postgres -d "$1" -At -c "$2"
}

timescale_stop() {
  docker rm -f "${TIMESCALE_CONTAINER}" > /dev/null 2>&1 || true
}

# Makes sure the existing TimescaleDB container (and the data loaded into it) is running.
# Returns non-zero if there is no such container.
timescale_resume() {
  if ! docker ps -a --format '{{.Names}}' | grep -qx "${TIMESCALE_CONTAINER}"; then
    return 1
  fi
  docker start "${TIMESCALE_CONTAINER}" > /dev/null
  for _ in $(seq 1 120); do
    if timescale_sql benchmark "SELECT 1" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done
  echo "TimescaleDB did not come back" >&2
  return 1
}

# Starts TimescaleDB with an empty data volume.
timescale_start_fresh() {
  timescale_stop
  docker volume rm -f tsbench-pgdata > /dev/null 2>&1 || true
  docker run -d --name "${TIMESCALE_CONTAINER}" --shm-size=2g \
    -p "127.0.0.1:${TIMESCALE_PORT}:5432" \
    -e POSTGRES_PASSWORD="${TIMESCALE_PASSWORD}" \
    -v tsbench-pgdata:/var/lib/postgresql/data \
    "${TIMESCALE_IMAGE}" > /dev/null
  # The image starts a temporary server to initialise the database, then restarts. It only
  # listens on TCP once that is over, so two successes in a row mean it is really up.
  local ok=0
  for _ in $(seq 1 300); do
    if timescale_sql postgres "SELECT 1" > /dev/null 2>&1; then
      ok=$((ok + 1))
      if [[ "${ok}" -ge 2 ]]; then
        return 0
      fi
    else
      ok=0
    fi
    sleep 0.5
  done
  echo "TimescaleDB did not start" >&2
  docker logs "${TIMESCALE_CONTAINER}" 2>&1 | tail -20 >&2
  return 1
}
