# tsengine

A time-series storage engine in C++20: write-ahead log, time-partitioned columnar blocks with
Gorilla compression, and aggregation push-down. Benchmarked against TimescaleDB on TSBS.

> **Status: the storage engine works; M3 (aggregation, HTTP API, TSBS ingest) is next.** Writes go
> through a WAL into time-partitioned, compressed block files, with compaction, retention and
> crash recovery. There is no network API yet and nothing has been benchmarked.
> Results, architecture diagram and limitations land here as the milestones do.

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

## Milestones

| | Deliverable | Status |
|---|---|---|
| M0 | Build, CI, design doc, line protocol parser, WAL write + replay | done |
| M1 | Memtable, flush to blocks, delta-of-delta + Gorilla encoders, range queries | done |
| M2 | Time partitions, retention, compaction, crash-recovery fault injection | done |
| M3 | Aggregations with push-down, HTTP API, TSBS ingest | |
| M4 | TSBS benchmarks vs TimescaleDB, plots | |
