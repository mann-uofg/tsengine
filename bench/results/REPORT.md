## Ingest

25,920,000 points, 4 loader workers, batches of 10000 rows, 5 runs from an empty database.

| System | Median points/s | Min | Max |
|---|---|---|---|
| tsengine | 6,007,496 | 5,837,739 | 6,268,371 |
| TimescaleDB | 5,207,612 | 4,997,456 | 5,373,242 |

## Storage

| System | Size on disk | Bytes per point |
|---|---|---|
| tsengine | 24.5 MB | 0.945 |
| TimescaleDB, compressed | 27.0 MB | 1.040 |
| TimescaleDB, uncompressed | 511.2 MB | 19.724 |

tsengine block files alone: 0.938 bytes per point. TimescaleDB compression took 4.3 s; tsengine's final compaction took 10.7 s.

## Query latency

Milliseconds, one client, warm cache. p50 / p99 over 5,000 timed queries per system and type.

| Query | tsengine | TimescaleDB compressed | TimescaleDB uncompressed | Answers compared | Mismatches |
|---|---|---|---|---|---|
| `single-groupby-1-1-1` | 0.21 / 0.40 | 1.06 / 1.50 | 1.19 / 1.66 | 2,000 | 0 |
| `single-groupby-1-1-12` | 0.35 / 0.67 | 2.32 / 2.99 | 4.95 / 6.28 | 2,000 | 0 |
| `single-groupby-1-8-1` | 0.31 / 0.52 | 1.59 / 2.27 | 5.12 / 6.30 | 2,000 | 0 |
| `cpu-max-all-1` | 0.71 / 1.11 | 2.37 / 3.22 | 3.83 / 4.70 | 2,000 | 0 |
| `double-groupby-1` | 7.72 / 9.74 | 27.24 / 33.99 | 31.14 / 40.22 | 2,000 | 0 |

- `single-groupby-1-1-1`: max of 1 metric, 1 host, per minute, over 1 hour.
- `single-groupby-1-1-12`: max of 1 metric, 1 host, per minute, over 12 hours.
- `single-groupby-1-8-1`: max of 1 metric, 8 hosts, per minute, over 1 hour.
- `cpu-max-all-1`: max of all 10 metrics, 1 host, per hour, over 8 hours.
- `double-groupby-1`: mean of 1 metric per host, all hosts, per hour, over 12 hours.

Check of this harness against the official TSBS runner (`tsbs_run_queries_timescaledb`, compressed table, median):

| Query | Official TSBS runner | This harness |
|---|---|---|
| `single-groupby-1-1-1` | 1.47 ms | 1.06 ms |
| `double-groupby-1` | 27.26 ms | 27.24 ms |

## Block size and aggregation push-down (tsengine)

| Points per block | Bytes per point | `double-groupby-1` p50 | blocks from statistics | `cpu-max-all-1` p50 | blocks from statistics | `single-groupby-1-1-12` p50 |
|---|---|---|---|---|---|---|
| 1024 | 0.945 | 7.72 ms | 0% | 0.71 ms | 0% | 0.35 ms |
| 360 | 1.076 | 1.46 ms | 85% | 0.29 ms | 78% | 0.36 ms |
| 128 | 1.533 | 3.14 ms | 64% | 0.40 ms | 62% | 0.37 ms |

## Environment

- date: 2026-10-11T17:55:51Z
- cpu: AMD Ryzen 7 7800X3D 8-Core Processor
- cpus: 16
- memory_kb: 15944272
- os: Ubuntu 24.04.5 LTS
- kernel: 6.18.40.1-microsoft-standard-WSL2
- filesystem: ext4
- docker: 29.1.3
- timescale_image: timescale/timescaledb:2.17.2-pg16
- tsbs_commit: 8323e59c74027b108f4ad5ec5d3e498b0101a02e
- tsengine_commit: 3caa24fa5198e6f9b5c0530b589099ca15cb231d
- tsengine_dirty: 0
- compiler: c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- go: go version go1.22.2 linux/amd64
- scale: 100
- ts_start: 2016-01-01T00:00:00Z
- ts_end: 2016-01-04T00:00:00Z
- log_interval: 10s
- timescaledb shared_buffers: 3892MB
- timescaledb container memory after load: 663.8MiB / 15.21GiB
- timescaledb container memory after queries: 687.9MiB / 15.21GiB
- tsengine peak resident memory during load: 116 MB
