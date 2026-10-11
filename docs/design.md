# tsengine design (v1)

Describes the storage engine as built: WAL, memtable, compressed block files, time partitions,
manifest, compaction, retention and crash recovery. Sections marked **planned** are not built
yet: tag filters, aggregation, the HTTP API and concurrent readers.

## Goal

A single-node time-series storage engine that ingests the TSBS cpu-only dataset, survives
`kill -9` without losing acknowledged writes, and answers range and aggregate queries fast enough
to compare honestly with TimescaleDB on the same hardware.

**Non-goals for v1:** SQL, replication or clustering, multi-field series, updates/deletes of
individual points, non-Linux platforms.

## Data model

- **Series** = measurement + sorted tag set + field name, written as one canonical key
  (`cpu,host=a,region=us usage_user`) and mapped to a `uint64` series ID. The mapping is kept in
  `series.log`, an append-only file in the WAL record format; each new series is fsynced there
  before any point can refer to its ID. Looking series up by tag (an inverted index) is
  **planned**.
- **Point** = `(int64 timestamp_ns, float64 value)`. One field per series; a line-protocol line
  with N fields becomes N series.

## Write path

```
Influx line protocol (HTTP ingest is planned)
  → parse → resolve series IDs
  → WAL append (+ fsync)                 ← write is acknowledged here
  → memtable insert
  → [memtable full] flush: one block file per time partition → drop the WAL segment
```

- **WAL:** append-only segment files of length-prefixed, CRC32C-checksummed records. One batch
  of points is one record, so a batch is recovered whole or not at all. Replay stops at the
  first torn or corrupt record. `sync_writes` chooses between fsync on every write (survives
  power loss) and no fsync (survives a process crash only). A new segment starts at every
  flush, so a segment always holds exactly the points of one memtable. Layout below.
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
  MANIFEST                    which block files are live, which WAL segments are still needed
  series.log                  series key → ID, append-only
  wal/00000007.wal            points not yet flushed
  blocks/00000042.tsb         immutable block file; holds points of one time partition
```

- **Time partitions.** Time is cut into fixed windows (1 day by default) and every block file
  holds points of exactly one window, as a TimescaleDB chunk does. A flush writes one file per
  partition the memtable touched, so late points simply become a new file in an old partition.
  Queries skip the files of partitions outside their time range.
- **Manifest.** The only source of truth for which block files exist. It lists each live file
  with its partition and size, and the lowest WAL segment still needed. It is small and is
  rewritten whole (temp file, fsync, rename, fsync directory) at the end of every flush,
  compaction and retention pass; that rename is the commit point of the operation. On open,
  any block file the manifest does not list is deleted.
- **Block file:** per series, blocks of up to 1024 points. Timestamps are delta-of-delta
  encoded, values Gorilla XOR encoded. An index at the end holds one entry per block: series
  ID, min/max timestamp, count, and sum/min/max of the values. Files are read with `mmap`.
  Layout below.
- **Compaction.** Merges files within one partition, never across partitions (time-window
  compaction, as in Cassandra TWCS). Within a partition the run to merge is chosen by size
  ratio, as in RocksDB universal compaction: starting from the newest file, the run grows
  backwards while the next older file is at most 1.2× the size of everything gathered so far,
  and is merged once it has at least `compaction_min_files` files. Big files are therefore
  rewritten rarely. The merge reads one series at a time from all inputs, newest write
  winning, and its output takes the inputs' place in the manifest.
- **Retention.** Dropping data means removing whole partitions from the manifest and deleting
  their files; no file is ever rewritten to expire data.

### Flush, step by step

Each step is safe to crash after, and the crash tests kill the process at every one of them:

1. Write one block file per partition. *Crash here:* the files are unlisted and are deleted on
   open; the WAL still has every point.
2. Create the next WAL segment. *Crash here:* an extra empty segment, which is harmless.
3. Save the manifest: the new files are live and the old segments are obsolete. This is the
   commit point. *Crash after it:* the points are in block files; the old segments are deleted
   on open.
4. Switch to the new segment, empty the memtable, delete the old segments.

Compaction follows the same shape: write the output, save the manifest with the output in place
of the inputs, then delete the inputs.

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

Built today: a query is one series ID and a time range.

1. Partition pruning: only files of partitions that overlap the range are considered.
2. Block pruning: within a file, a binary search on the index finds the blocks whose time range
   overlaps the query; no other block is touched.
3. Each of those blocks is checksummed, decoded and trimmed to the range.
4. Results are merged across files, oldest first, and then with the memtable. Where the same
   timestamp appears more than once, the newest source wins.

**Planned:** a series selector (measurement + tag filters) in front of step 1, and aggregates
(`avg/min/max/sum/count` per time bucket) with push-down: a block that lies entirely inside one
bucket is answered from its index entry and never decoded.

## Concurrency

Today the engine is single-threaded: one thread writes and queries. **Planned:** concurrent
readers on ref-counted snapshots of the file set (LevelDB `VersionSet` style), so a flush or
compaction never pulls a file out from under a running query.

## Recovery

Opening a database always runs the same steps, whatever the previous process was doing when it
stopped:

1. Load the manifest. A missing manifest means a new database; a damaged one is an error.
2. Delete block files the manifest does not list, leftover temporary files, and WAL segments
   below the manifest's low-water mark.
3. Replay `series.log` to rebuild the key → ID map, cutting off a torn last record.
4. Replay the remaining WAL segments, oldest first, into the memtable. A torn record at the end
   of the newest segment is cut off; a bad record anywhere else is corruption and stops the
   open.

## Durability contract

A write is acknowledged only after its WAL record has been written, and fsynced if `sync_writes`
is on. Flush and compaction write new files, fsync them, then atomically replace the manifest; a
crash at any point leaves either the old state or the new one, never a mix.

This is tested by killing a writer process and reopening the database: with SIGKILL at random
moments, and by exiting at each step of a write, a flush and a compaction. After every kill, all
acknowledged batches must be present and correct, nothing else may be present, every batch must
be there for all of its series or none, and no stray files may remain. Killing a process does
not discard what the kernel has already been given, so these tests show recovery from a crash at
any point; they do not show survival of power loss, which rests on the fsync ordering above.
