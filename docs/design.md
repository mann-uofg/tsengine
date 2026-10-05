# tsengine design (v1)

Draft. Sections marked **open** are decided when the component is built.

## Goal

A single-node time-series storage engine that ingests the TSBS cpu-only dataset, survives
`kill -9` without losing acknowledged writes, and answers range and aggregate queries fast enough
to compare honestly with TimescaleDB on the same hardware.

**Non-goals for v1:** SQL, replication or clustering, multi-field series, updates/deletes of
individual points, non-Linux platforms.

## Data model

- **Series** = measurement + sorted tag set, mapped to a `uint64` series ID by a persisted
  inverted index (tag pair → series IDs).
- **Point** = `(int64 timestamp_ns, float64 value)`. One field per series; a line-protocol line
  with N fields becomes N series.

## Write path

```
HTTP (Influx line protocol)
  → parse → resolve series IDs
  → WAL append (+ fsync per policy)      ← write is acknowledged here
  → memtable insert
  → [size threshold] flush to an immutable block file → WAL truncate
```

- **WAL:** append-only segment files of length-prefixed, CRC32C-checksummed records. Replay stops
  at the first torn or corrupt record. fsync policy is configurable: per write, group commit, or
  interval. Layout below.
- **Memtable:** per-series append buffers. Out-of-order points are accepted within a bounded
  window and sorted at flush. Structure: **open**.

### WAL format

All integers are fixed-width little-endian.

Segment file `wal/NNNNNNNN.wal`:

```
segment header (8 bytes):  magic u32 "TSWL" | version u16 | reserved u16
then records back to back:
  crc32c  u32   over length, type and payload
  length  u32   payload bytes, at most 16 MiB
  type    u8    1 = SeriesDef, 2 = Samples; 0 is never valid
  payload
```

| Record | Payload |
|---|---|
| `SeriesDef` | `series_id u64`, `key_len u32`, `key` bytes (canonical series key) |
| `Samples` | repeated `series_id u64`, `timestamp_ns i64`, `value` (IEEE-754 bits, `u64`) — 24 bytes per point, count = length / 24 |

Replay reads records in order and stops at the first one that is short, has an invalid type or
length, or fails its CRC. In the newest segment that is a torn tail: the file is truncated to the
end of the last good record and appends continue from there. In any older segment it is
corruption and startup fails.

## Storage layout

```
data/
  wal/000001.wal ...
  index/                      series index
  2026-10-05/                 one directory per time partition (1 day)
    000007.tsb                immutable block file
  MANIFEST                    which files are live
```

- **Time partitions** are the unit of retention (drop the directory) and of compaction, as
  TimescaleDB chunks are.
- **Block file:** per series, blocks of ~1k points. Timestamps are delta-of-delta encoded, values
  Gorilla XOR encoded. A footer holds one index entry per block: series ID, min/max timestamp,
  count, and sum/min/max of the values. Files are read with `mmap`. Byte layout: **open**.
- **Compaction:** time-window (Cassandra TWCS style) — merge the small flushed files inside one
  partition into one, resolving duplicate timestamps (last write wins).

## Read path

Query = series selector (measurement + tag filters) + time range + optional aggregate
(`avg/min/max/sum/count`) with a bucket width.

1. Index lookup: selector → series IDs.
2. Partition pruning by time range.
3. Block pruning by footer min/max timestamp.
4. **Aggregation push-down:** a block that lies entirely inside one bucket is answered from its
   footer stats and is never decoded.
5. Decode the remaining blocks and merge with the memtable.

## Concurrency

One writer. Readers take a ref-counted snapshot of the manifest (LevelDB `VersionSet` style), so
flush and compaction never invalidate a running query.

## Durability contract

A write is acknowledged only after its WAL record is durable under the configured fsync policy.
Flush and compaction write new files, fsync them, then atomically switch the manifest; a crash at
any point leaves either the old state or the new one, never a mix. Proven by fault-injection
tests that `kill -9` the process during writes, flushes and compactions.
