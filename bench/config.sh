#!/usr/bin/env bash
#
# Settings shared by the benchmark scripts. Sourced, not run.

# Where builds, tools, data and results live. Must be a native Linux filesystem.
WORK="${TSBENCH_WORK:-$HOME/tsbench}"

# TSBS at a fixed commit, so the workload does not change under the results.
TSBS_COMMIT="${TSBS_COMMIT:-8323e59c74027b108f4ad5ec5d3e498b0101a02e}"

# The dataset: TSBS cpu-only. Every host reports 10 CPU metrics every 10 seconds.
SEED=123
SCALE=100
TS_START="2016-01-01T00:00:00Z"
TS_END="2016-01-04T00:00:00Z"
LOG_INTERVAL="10s"
DATA_INFLUX="${WORK}/data/cpu.influx"
DATA_TIMESCALE="${WORK}/data/cpu.timescaledb"

# The baseline.
TIMESCALE_IMAGE="timescale/timescaledb:2.17.2-pg16"
TIMESCALE_CONTAINER="tsbench-timescaledb"
TIMESCALE_PORT=15432
TIMESCALE_PASSWORD="tsbench"

# tsengine.
TSENGINE_SERVER="${WORK}/build-release/tsengine_server"
TSENGINE_PORT=18086
TSENGINE_URL="http://127.0.0.1:${TSENGINE_PORT}"

# Loading: the same client settings for both systems.
LOAD_WORKERS=4
LOAD_BATCH_SIZE=10000
