# tsengine design (v1)

Describes what is built so far (WAL, block files, memtable, range queries) and the plan for the
rest. Time partitions, the manifest, compaction, the series index and the HTTP API are not
built yet.

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
- **Memtable:** a hash map from series ID to an append buffer of timestamps and values.
  Appending never sorts. A series is sorted only when it is read or flushed, and only if points
  actually arrived out of order; a timestamp written more than once keeps its last write.

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

Replay reads records in order and stops at the first one that is short, has an impossible
length, or fails its CRC. In the newest segment that is a torn tail: the file is truncated to the
end of the last good record and appends continue from there. In any older segment it is
corruption and startup fails. A record that passes its CRC but has an unknown type or a
malformed payload is always corruption.

A segment is created by writing its header to a temporary file, fsyncing it, renaming it into
place and fsyncing the directory, so a file with a segment name always has a complete header.
After any failed write or fsync the writer refuses further appends.

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
  count, and sum/min/max of the values. Files are read with `mmap`. Layout below.
- **Compaction:** time-window (Cassandra TWCS style) — merge the small flushed files inside one
  partition into one, resolving duplicate timestamps (last write wins).

### Block file format

All integers are fixed-width little-endian.

```
header (8 bytes):   magic u32 "TSBK" | version u16 | reserved u16
blocks:             for each block, its timestamp stream then its value stream
index:              one 72-byte entry per block, sorted by (series ID, min timestamp)
footer (24 bytes):  index offset u64 | block count u64 | index crc32c u32 | magic u32
```

Index entry:

| Field | Type | |
|---|---|---|
| series ID | u64 | |
| min timestamp, max timestamp | i64, i64 | used to skip blocks outside a query's range |
| offset, size | u64, u32 | where the block is in the file |
| timestamp size | u32 | bytes of the timestamp stream; the rest is values |
| count | u32 | points in the block, at most 1024 |
| crc32c | u32 | of the block's bytes, checked on every read |
| sum, min, max | f64 × 3 | of the block's values, for aggregation push-down |

A file is written under a temporary name and renamed into place only after it is complete and
fsynced, so a block file that exists is always whole. Blocks of one series never overlap in time
within a file, which is what makes the index binary-searchable.

### Block encoding

Both streams follow the Gorilla paper (Pelkonen et al., VLDB 2015, §4.1), stored as two separate
columns instead of interleaved. The point count lives in the index entry, not in the stream.

**Timestamps — delta-of-delta.** The first timestamp is stored whole. Each later one stores
`D = (t[i] - t[i-1]) - (t[i-1] - t[i-2])`, the change in the gap between points:

| D | Encoding |
|---|---|
| 0 | `0` |
| fits in 16 bits | `10` + 16 bits |
| fits in 32 bits | `110` + 32 bits |
| fits in 48 bits | `1110` + 48 bits |
| otherwise | `1111` + 64 bits |

Gorilla's buckets are 7/9/12/32 bits because its timestamps are in seconds; these are wider
because ours are in nanoseconds. A series sampled at a fixed interval costs 1 bit per point.

**Values — XOR.** The first value is stored whole. Each later one is XORed with its predecessor:

| XOR | Encoding |
|---|---|
| 0 (value unchanged) | `0` |
| meaningful bits fit inside the previous window | `10` + those bits |
| otherwise | `11` + leading zeros (5 bits) + bit count − 1 (6 bits) + the bits |

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
