# tsengine

A time-series storage engine in C++20: write-ahead log, time-partitioned columnar blocks with
Gorilla compression, and aggregation push-down. Benchmarked against TimescaleDB on TSBS.

> **Status: everything but the benchmarks.** The engine ingests Influx line protocol over HTTP,
> stores it in time-partitioned compressed block files behind a WAL, and answers range and
> aggregate queries. CI loads a TSBS dataset with the stock TSBS loader and verifies it.
> **Nothing has been benchmarked yet**; the comparison against TimescaleDB is the last
> milestone, and this page gets its results table, architecture diagram and limitations then.

## Results

_Pending — TSBS comparison against TimescaleDB arrives in M4._

## Design

See [docs/design.md](docs/design.md).

## Build and test

Linux (or WSL2), CMake ≥ 3.25, Ninja, GCC ≥ 12 or Clang ≥ 15.

```bash
cmake --preset debug          # Debug + ASan/UBSan
cmake --build --preset debug
ctest --preset debug
```

Use the `release` preset for benchmarks.

## Run

```bash
cmake --preset release && cmake --build --preset release
build/release/tsengine_server --dir ./data --port 8086
```

Write points as Influx line protocol, then query them:

```bash
curl -X POST 'http://127.0.0.1:8086/write' --data-binary \
  'cpu,host=a usage=58 1451606400000000000
cpu,host=a usage=61 1451606410000000000
cpu,host=b usage=12 1451606400000000000'

# Raw points of one series
curl 'http://127.0.0.1:8086/api/v1/query?measurement=cpu&field=usage&tag=host:a'

# Maximum per minute across all hosts, and mean per host over everything
curl 'http://127.0.0.1:8086/api/v1/query?measurement=cpu&agg=max&bucket=60000000000'
curl 'http://127.0.0.1:8086/api/v1/query?measurement=cpu&agg=mean&group_by=host'
```

The server is also a drop-in target for `tsbs_load_influx`; see
[scripts/tsbs_smoke.sh](scripts/tsbs_smoke.sh) for the exact commands CI runs.

## Milestones

| | Deliverable | Status |
|---|---|---|
| M0 | Build, CI, design doc, line protocol parser, WAL write + replay | done |
| M1 | Memtable, flush to blocks, delta-of-delta + Gorilla encoders, range queries | done |
| M2 | Time partitions, retention, compaction, crash-recovery fault injection | done |
| M3 | Aggregations with push-down, HTTP API, TSBS ingest | done |
| M4 | TSBS benchmarks vs TimescaleDB, plots | |
