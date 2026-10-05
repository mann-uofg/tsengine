# tsengine

A time-series storage engine in C++20: write-ahead log, time-partitioned columnar blocks with
Gorilla compression, and aggregation push-down. Benchmarked against TimescaleDB on TSBS.

> **Status: M0 (setup + design).** Nothing below the build instructions is implemented yet.
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
| M0 | Build, CI, design doc, line protocol parser, WAL write + replay | in progress |
| M1 | Memtable, flush to blocks, delta-of-delta + Gorilla encoders, range queries | |
| M2 | Time partitions, retention, compaction, crash-recovery fault injection | |
| M3 | Aggregations with push-down, HTTP API, TSBS ingest | |
| M4 | TSBS benchmarks vs TimescaleDB, plots | |
