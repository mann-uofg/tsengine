#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tsengine/block_file.h"
#include "tsengine/manifest.h"
#include "tsengine/memtable.h"
#include "tsengine/status.h"
#include "tsengine/types.h"
#include "tsengine/wal.h"

namespace tsengine {

struct EngineOptions {
  // Width of a time partition. Fixed when the database is created; opening an existing
  // database with a different value is refused.
  int64_t partition_duration_ns = int64_t{86400} * 1000000000;

  // The memtable is flushed to block files once it holds this many points.
  size_t memtable_flush_points = 1000000;

  // If true, a write is fsynced to the WAL before Write() returns, so it survives power
  // loss. If false, a returned write survives a crash of the process but not of the machine.
  bool sync_writes = true;

  // A partition is compacted when a run of at least this many similar-sized files has built
  // up in it. 0 or 1 turns automatic compaction off.
  size_t compaction_min_files = 4;

  // For tests only. Called with a name at certain points inside flush, compaction and
  // write, so a test can kill the process exactly there.
  std::function<void(std::string_view)> test_hook;
};

// The storage engine: a directory holding a write-ahead log, a set of immutable block files
// and a manifest that says which files are live.
//
//   <dir>/MANIFEST            which block files are live, and which WAL segments are needed
//   <dir>/series.log          every series key and the id it was given; append-only
//   <dir>/wal/NNNNNNNN.wal    points not yet flushed
//   <dir>/blocks/NNNNNNNN.tsb flushed, compressed points, one time partition per file
//
// Not thread-safe: one thread at a time.
class Engine {
 public:
  // Opens the database in `dir`, creating it if it does not exist, and recovers whatever
  // state a previous process left behind.
  static Status Open(const std::string& dir, const EngineOptions& options,
                     std::unique_ptr<Engine>* out);

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Returns the id of the series with this key, assigning a new one if the key is new. A
  // new assignment is on disk before this returns.
  Status GetOrCreateSeries(std::string_view key, uint64_t* series_id);

  // Looks a key up without creating it.
  bool FindSeries(std::string_view key, uint64_t* series_id) const;

  // Stores a batch of points. When this returns OK the batch is in the WAL and will survive
  // a crash; a batch is recovered completely or not at all.
  Status Write(std::span<const Sample> samples);

  // Replaces `out` with the points of one series inside `range`, in time order.
  Status Query(uint64_t series_id, TimeRange range, Points* out) const;

  // Writes the memtable to block files and discards the WAL segments that covered it.
  Status Flush();

  // Merges the files of every partition that has more than one, down to one file each.
  Status Compact();

  // Retention: drops every partition that lies entirely before the partition containing
  // `timestamp_ns`. Only flushed data is affected.
  Status DropPartitionsBefore(int64_t timestamp_ns);

  // The live block files, in manifest order.
  std::vector<FileEntry> Files() const;
  size_t memtable_points() const { return memtable_.point_count(); }
  size_t series_count() const { return series_ids_.size(); }
  uint64_t wal_segment() const { return wal_segment_; }

 private:
  struct LiveFile {
    FileEntry entry;
    std::unique_ptr<BlockFileReader> reader;
  };

  // Lets the series map be searched with a string_view, without building a std::string.
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); }
  };

  Engine(std::string dir, EngineOptions options);

  Status Recover();
  Status RecoverManifest();
  Status RecoverSeries();
  Status RecoverWal();

  Status FlushMemtableToFiles();
  Status CompactRun(size_t begin, size_t end);
  Status CompactWherePolicySays();
  Status CompactEverything();
  size_t PartitionEnd(size_t begin) const;
  size_t PickCompactionRun(size_t begin, size_t end) const;

  // Makes `entries` the live set of block files. Saving the manifest is the commit point;
  // after it, files that are no longer listed are closed and deleted.
  Status Commit(std::vector<FileEntry> entries, uint64_t min_wal_segment, std::string_view hook);

  // Remembers a failure. After one, the engine refuses to change anything: its in-memory
  // state may no longer match the disk, and reopening the database is the way to recover.
  Status Guard(Status s);

  int64_t PartitionOf(int64_t timestamp_ns) const;
  std::string ManifestPath() const;
  std::string SeriesLogPath() const;
  std::string WalPath(uint64_t number) const;
  std::string BlockPath(uint64_t number) const;
  void Hook(std::string_view point) const;

  std::string dir_;
  EngineOptions options_;
  Status fatal_;

  // Sorted by partition, oldest first within a partition.
  std::vector<LiveFile> files_;
  uint64_t next_file_number_ = 1;

  Memtable memtable_;
  std::unique_ptr<WalWriter> wal_;
  uint64_t wal_segment_ = 1;
  uint64_t min_wal_segment_ = 1;

  std::unique_ptr<WalWriter> series_log_;
  std::unordered_map<std::string, uint64_t, StringHash, std::equal_to<>> series_ids_;
  uint64_t next_series_id_ = 1;
};

}  // namespace tsengine
