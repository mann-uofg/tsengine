#!/usr/bin/env bash
#
# Turns on TimescaleDB's native compression for the loaded table and compresses every chunk.
#
# The settings are the ones TimescaleDB documents for data of this shape: one compressed
# segment per series (segment by the series id) with rows ordered by time.
#
# usage: compress.sh <label>

set -euo pipefail

source "$(dirname "$0")/lib.sh"

LABEL="$1"
OUT="${WORK}/results/compress-timescaledb-${LABEL}.txt"
: > "${OUT}"

record() {
  echo "$1=$2" | tee -a "${OUT}"
}

timescale_resume
record bytes_uncompressed "$(timescale_sql benchmark "SELECT hypertable_size('cpu')")"

start_ns="$(date +%s%N)"
timescale_sql benchmark "ALTER TABLE cpu SET (timescaledb.compress,
  timescaledb.compress_segmentby = 'tags_id',
  timescaledb.compress_orderby = 'time DESC')" > /dev/null
timescale_sql benchmark "SELECT count(compress_chunk(c)) FROM show_chunks('cpu') c" > /dev/null
record compress_seconds "$(awk "BEGIN {print ($(date +%s%N) - ${start_ns}) / 1e9}")"
timescale_sql benchmark "VACUUM ANALYZE" > /dev/null

record bytes_compressed "$(timescale_sql benchmark "SELECT hypertable_size('cpu')")"
record chunks "$(timescale_sql benchmark "SELECT count(*) FROM show_chunks('cpu')")"
record compressed_chunks "$(timescale_sql benchmark \
  "SELECT count(*) FROM timescaledb_information.chunks WHERE is_compressed")"
record rows_stored "$(timescale_sql benchmark "SELECT count(*) FROM cpu")"
