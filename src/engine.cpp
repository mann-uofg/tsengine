#include "tsengine/engine.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <map>
#include <system_error>
#include <utility>

#include "tsengine/file_util.h"
#include "tsengine/query.h"

namespace tsengine {
namespace {

constexpr std::string_view kWalExtension = ".wal";
constexpr std::string_view kBlockExtension = ".tsb";
constexpr std::string_view kTempExtension = ".tmp";

// Division that rounds towards negative infinity, so timestamps before 1970 land in the
// right partition. `b` must be positive.
int64_t FloorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if (a % b != 0 && a < 0) {
    --q;
  }
  return q;
}

// 42 -> "00000042.wal"
std::string NumberedName(uint64_t number, std::string_view extension) {
  const std::string digits = std::to_string(number);
  std::string name(digits.size() < 8 ? 8 - digits.size() : 0, '0');
  name += digits;
  name += extension;
  return name;
}

// "00000042.wal" -> 42. Returns false for a name of any other shape.
bool ParseNumberedName(std::string_view name, std::string_view extension, uint64_t* number) {
  if (name.size() <= extension.size() || !name.ends_with(extension)) {
    return false;
  }
  const char* begin = name.data();
  const char* end = begin + (name.size() - extension.size());
  const auto [ptr, ec] = std::from_chars(begin, end, *number);
  return ec == std::errc() && ptr == end;
}

void RemoveFile(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

Status FileSize(const std::string& path, uint64_t* size) {
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec) {
    return Status::IOError("stat " + path + ": " + ec.message());
  }
  *size = bytes;
  return Status::OK();
}

// Collects the numbers of the files named NNNNNNNN<extension> in `dir`, in increasing order.
// Temporary files are leftovers from a write that never finished, and are deleted.
Status ScanDir(const std::string& dir, std::string_view extension, std::vector<uint64_t>* out) {
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  const std::filesystem::directory_iterator end;
  while (!ec && it != end) {
    const std::string name = it->path().filename().string();
    uint64_t number = 0;
    if (name.ends_with(kTempExtension)) {
      RemoveFile(it->path().string());
    } else if (ParseNumberedName(name, extension, &number)) {
      out->push_back(number);
    }
    it.increment(ec);
  }
  if (ec) {
    return Status::IOError("list " + dir + ": " + ec.message());
  }
  std::sort(out->begin(), out->end());
  return Status::OK();
}

// Copies points [begin, end) of `from` into `to`.
void CopySlice(const Points& from, size_t begin, size_t end, Points* to) {
  const std::span<const int64_t> timestamps(from.timestamps);
  const std::span<const double> values(from.values);
  const auto t = timestamps.subspan(begin, end - begin);
  const auto v = values.subspan(begin, end - begin);
  to->timestamps.assign(t.begin(), t.end());
  to->values.assign(v.begin(), v.end());
}

// Inserts `entry` after the last file of its partition, keeping the list ordered by
// partition with the oldest file of each partition first.
void InsertEntry(std::vector<FileEntry>* entries, const FileEntry& entry) {
  auto pos = entries->begin();
  while (pos != entries->end() && pos->partition <= entry.partition) {
    ++pos;
  }
  entries->insert(pos, entry);
}

// Rebuilds the memtable from WAL segments.
class SamplesToMemtable : public WalReplayHandler {
 public:
  explicit SamplesToMemtable(Memtable* memtable) : memtable_(memtable) {}

  void OnSeriesDef(uint64_t, std::string_view) override {}

  void OnSamples(std::span<const Sample> samples) override {
    for (const Sample& sample : samples) {
      memtable_->Append(sample.series_id, sample.timestamp_ns, sample.value);
    }
  }

 private:
  Memtable* memtable_;
};

// Collects the series definitions of the series log.
class SeriesCollector : public WalReplayHandler {
 public:
  void OnSeriesDef(uint64_t series_id, std::string_view key) override {
    series.emplace_back(std::string(key), series_id);
  }

  void OnSamples(std::span<const Sample>) override {}

  std::vector<std::pair<std::string, uint64_t>> series;
};

}  // namespace

Engine::Engine(std::string dir, EngineOptions options)
    : dir_(std::move(dir)), options_(std::move(options)) {}

Status Engine::Open(const std::string& dir, const EngineOptions& options,
                    std::unique_ptr<Engine>* out) {
  if (options.partition_duration_ns <= 0) {
    return Status::InvalidArgument("partition duration must be positive");
  }
  std::unique_ptr<Engine> engine(new Engine(dir, options));
  Status s = engine->Recover();
  if (!s.ok()) {
    return s;
  }
  *out = std::move(engine);
  return Status::OK();
}

Status Engine::Recover() {
  std::error_code ec;
  std::filesystem::create_directories(dir_ + "/wal", ec);
  if (!ec) {
    std::filesystem::create_directories(dir_ + "/blocks", ec);
  }
  if (ec) {
    return Status::IOError("create " + dir_ + ": " + ec.message());
  }
  Status s = RecoverManifest();
  if (s.ok()) {
    s = RecoverSeries();
  }
  if (s.ok()) {
    s = RecoverWal();
  }
  return s;
}

Status Engine::RecoverManifest() {
  Manifest manifest;
  bool found = false;
  Status s = LoadManifest(ManifestPath(), &manifest, &found);
  if (!s.ok()) {
    return s;
  }
  if (!found) {
    // A new database. Saving the manifest also syncs the directory, which makes the wal/
    // and blocks/ directories created above durable.
    manifest.partition_duration_ns = options_.partition_duration_ns;
    s = SaveManifest(ManifestPath(), manifest);
    if (!s.ok()) {
      return s;
    }
  } else if (manifest.partition_duration_ns != options_.partition_duration_ns) {
    return Status::InvalidArgument(dir_ + ": database was created with another partition size");
  }
  next_file_number_ = manifest.next_file_number;
  min_wal_segment_ = manifest.min_wal_segment;

  // A block file that the manifest does not list is the output of a flush or compaction
  // that crashed before it could commit, or an input of one that crashed just after.
  // Either way it is not part of the database.
  std::vector<uint64_t> live;
  for (const FileEntry& entry : manifest.files) {
    live.push_back(entry.number);
  }
  std::sort(live.begin(), live.end());
  std::vector<uint64_t> on_disk;
  s = ScanDir(dir_ + "/blocks", kBlockExtension, &on_disk);
  if (!s.ok()) {
    return s;
  }
  for (const uint64_t number : on_disk) {
    if (!std::binary_search(live.begin(), live.end(), number)) {
      RemoveFile(BlockPath(number));
    }
  }

  for (const FileEntry& entry : manifest.files) {
    LiveFile file;
    file.entry = entry;
    s = BlockFileReader::Open(BlockPath(entry.number), &file.reader);
    if (!s.ok()) {
      return s;
    }
    files_.push_back(std::move(file));
  }
  return Status::OK();
}

Status Engine::RecoverSeries() {
  const std::string path = SeriesLogPath();
  // Series definitions are always synced: a point must never refer to an id whose key
  // could be lost.
  WalOptions wal_options;
  wal_options.sync_on_append = true;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return WalWriter::Create(path, wal_options, &series_log_);
  }
  SeriesCollector collector;
  WalReplayResult result;
  Status s = ReplayWalSegment(path, &collector, &result);
  if (!s.ok()) {
    return s;
  }
  for (auto& entry : collector.series) {
    next_series_id_ = std::max(next_series_id_, entry.second + 1);
    series_ids_.emplace(std::move(entry.first), entry.second);
  }
  // A torn record at the end is a series that was being created when the process died. No
  // point can refer to it yet, so it is simply cut off.
  return WalWriter::OpenExisting(path, result.valid_size, wal_options, &series_log_);
}

Status Engine::RecoverWal() {
  std::vector<uint64_t> segments;
  Status s = ScanDir(dir_ + "/wal", kWalExtension, &segments);
  if (!s.ok()) {
    return s;
  }
  WalOptions wal_options;
  wal_options.sync_on_append = options_.sync_writes;
  SamplesToMemtable loader(&memtable_);
  WalReplayResult result;
  bool replayed = false;
  for (size_t i = 0; i < segments.size(); ++i) {
    const uint64_t number = segments[i];
    if (number < min_wal_segment_) {
      // Already flushed; the crash came before the segment could be deleted.
      RemoveFile(WalPath(number));
      continue;
    }
    s = ReplayWalSegment(WalPath(number), &loader, &result);
    if (!s.ok()) {
      return s;
    }
    // Only the newest segment was being written when the process died. A bad record in an
    // older one is damage to data that was already safely on disk.
    const bool newest = i + 1 == segments.size();
    if (result.torn_tail && !newest) {
      return Status::Corruption(WalPath(number) + ": " + result.tail_reason);
    }
    wal_segment_ = number;
    replayed = true;
  }
  if (!replayed) {
    wal_segment_ = min_wal_segment_;
    return WalWriter::Create(WalPath(wal_segment_), wal_options, &wal_);
  }
  return WalWriter::OpenExisting(WalPath(wal_segment_), result.valid_size, wal_options, &wal_);
}

Status Engine::GetOrCreateSeries(std::string_view key, uint64_t* series_id) {
  if (FindSeries(key, series_id)) {
    return Status::OK();
  }
  if (!fatal_.ok()) {
    return fatal_;
  }
  Status s = series_log_->AppendSeriesDef(next_series_id_, key);
  if (!s.ok()) {
    if (s.code() == Status::Code::kIOError) {
      fatal_ = s;
    }
    return s;
  }
  *series_id = next_series_id_;
  series_ids_.emplace(std::string(key), next_series_id_);
  ++next_series_id_;
  return Status::OK();
}

bool Engine::FindSeries(std::string_view key, uint64_t* series_id) const {
  const auto it = series_ids_.find(key);
  if (it == series_ids_.end()) {
    return false;
  }
  *series_id = it->second;
  return true;
}

Status Engine::Write(std::span<const Sample> samples) {
  if (!fatal_.ok()) {
    return fatal_;
  }
  if (samples.empty()) {
    return Status::OK();
  }
  // Log first. The batch only reaches the memtable, and the caller only hears OK, once it
  // is in the WAL.
  Status s = wal_->AppendSamples(samples);
  if (!s.ok()) {
    // A batch that was rejected for its size left the log untouched. An I/O error did not:
    // the log may now end in half a record, so nothing more may be appended to it.
    if (s.code() == Status::Code::kIOError) {
      fatal_ = s;
    }
    return s;
  }
  Hook("write:wal_appended");
  for (const Sample& sample : samples) {
    memtable_.Append(sample.series_id, sample.timestamp_ns, sample.value);
  }
  if (memtable_.point_count() >= options_.memtable_flush_points) {
    return Flush();
  }
  return Status::OK();
}

Status Engine::Query(uint64_t series_id, TimeRange range, Points* out) const {
  out->clear();
  if (range.min > range.max) {
    return Status::OK();
  }
  // Partition pruning: a file can only hold points of its own partition, so files of
  // partitions outside the range are not even opened.
  const int64_t first = PartitionOf(range.min);
  const int64_t last = PartitionOf(range.max);
  std::vector<const BlockFileReader*> inputs;
  for (const LiveFile& file : files_) {
    if (file.entry.partition >= first && file.entry.partition <= last) {
      inputs.push_back(file.reader.get());
    }
  }
  return QueryRange(inputs, &memtable_, series_id, range, out);
}

Status Engine::Flush() {
  if (!fatal_.ok()) {
    return fatal_;
  }
  if (memtable_.empty()) {
    return Status::OK();
  }
  Status s = Guard(FlushMemtableToFiles());
  if (s.ok()) {
    s = Guard(CompactWherePolicySays());
  }
  return s;
}

Status Engine::Compact() {
  if (!fatal_.ok()) {
    return fatal_;
  }
  return Guard(CompactEverything());
}

Status Engine::DropPartitionsBefore(int64_t timestamp_ns) {
  if (!fatal_.ok()) {
    return fatal_;
  }
  const int64_t cutoff = PartitionOf(timestamp_ns);
  std::vector<FileEntry> kept;
  for (const LiveFile& file : files_) {
    if (file.entry.partition >= cutoff) {
      kept.push_back(file.entry);
    }
  }
  if (kept.size() == files_.size()) {
    return Status::OK();
  }
  return Guard(Commit(std::move(kept), min_wal_segment_, "retention:manifest_saved"));
}

std::vector<FileEntry> Engine::Files() const {
  std::vector<FileEntry> entries;
  for (const LiveFile& file : files_) {
    entries.push_back(file.entry);
  }
  return entries;
}

// Flush, step by step. A crash between any two steps must leave a database that opens and
// has every acknowledged point:
//
//   1. Write one block file per partition.       Crash: files are unlisted, so deleted at
//                                                 startup. The WAL still has the points.
//   2. Create the next WAL segment.              Crash: an extra empty segment; harmless.
//   3. Save the manifest: new files are live,    Crash before: as above. Crash after: the
//      old WAL segments are obsolete.            points are in block files; old segments
//                                                 are deleted at startup.
//   4. Switch to the new segment, empty the memtable, delete the old segments.
Status Engine::FlushMemtableToFiles() {
  struct Output {
    uint64_t number = 0;
    std::unique_ptr<BlockFileWriter> writer;
  };
  std::map<int64_t, Output> outputs;

  // Series are visited in increasing id order, and each one is cut where it crosses a
  // partition boundary, so every output file receives its series in increasing id order.
  Points points;
  Points slice;
  for (const uint64_t series_id : memtable_.SeriesIds()) {
    points.clear();
    memtable_.ReadRange(series_id, TimeRange(), &points);
    size_t begin = 0;
    while (begin < points.size()) {
      const int64_t partition = PartitionOf(points.timestamps[begin]);
      size_t end = begin + 1;
      while (end < points.size() && PartitionOf(points.timestamps[end]) == partition) {
        ++end;
      }
      Output& output = outputs[partition];
      if (output.writer == nullptr) {
        output.number = next_file_number_++;
        Status s = BlockFileWriter::Create(BlockPath(output.number), &output.writer);
        if (!s.ok()) {
          return s;
        }
      }
      CopySlice(points, begin, end, &slice);
      Status s = output.writer->AddSeries(series_id, slice);
      if (!s.ok()) {
        return s;
      }
      begin = end;
    }
  }

  std::vector<FileEntry> entries = Files();
  for (auto& [partition, output] : outputs) {
    Status s = output.writer->Finish();
    if (!s.ok()) {
      return s;
    }
    FileEntry entry;
    entry.number = output.number;
    entry.partition = partition;
    s = FileSize(BlockPath(entry.number), &entry.size);
    if (!s.ok()) {
      return s;
    }
    InsertEntry(&entries, entry);
  }
  Hook("flush:blocks_written");

  WalOptions wal_options;
  wal_options.sync_on_append = options_.sync_writes;
  std::unique_ptr<WalWriter> next_wal;
  const uint64_t next_segment = wal_segment_ + 1;
  Status s = WalWriter::Create(WalPath(next_segment), wal_options, &next_wal);
  if (!s.ok()) {
    return s;
  }
  Hook("flush:wal_rotated");

  const uint64_t first_obsolete = min_wal_segment_;
  s = Commit(std::move(entries), next_segment, "flush:manifest_saved");
  if (!s.ok()) {
    return s;
  }
  wal_ = std::move(next_wal);
  wal_segment_ = next_segment;
  memtable_ = Memtable();
  for (uint64_t number = first_obsolete; number < next_segment; ++number) {
    RemoveFile(WalPath(number));
  }
  return Status::OK();
}

Status Engine::Commit(std::vector<FileEntry> entries, uint64_t min_wal_segment,
                      std::string_view hook) {
  Manifest manifest;
  manifest.partition_duration_ns = options_.partition_duration_ns;
  manifest.next_file_number = next_file_number_;
  manifest.min_wal_segment = min_wal_segment;
  manifest.files = std::move(entries);
  Status s = SaveManifest(ManifestPath(), manifest);
  if (!s.ok()) {
    return s;
  }
  Hook(hook);
  min_wal_segment_ = min_wal_segment;

  // The change is now permanent. Build the new list of open files, reusing the readers of
  // files that stay and opening the ones that are new.
  std::vector<LiveFile> next;
  next.reserve(manifest.files.size());
  for (const FileEntry& entry : manifest.files) {
    LiveFile file;
    file.entry = entry;
    for (LiveFile& old : files_) {
      if (old.entry.number == entry.number) {
        file.reader = std::move(old.reader);
        break;
      }
    }
    if (file.reader == nullptr) {
      s = BlockFileReader::Open(BlockPath(entry.number), &file.reader);
      if (!s.ok()) {
        return s;
      }
    }
    next.push_back(std::move(file));
  }
  // Whatever still has its reader was not carried over: it is no longer part of the
  // database. Unmap it and delete it.
  for (LiveFile& old : files_) {
    if (old.reader != nullptr) {
      old.reader.reset();
      RemoveFile(BlockPath(old.entry.number));
    }
  }
  files_ = std::move(next);
  return Status::OK();
}

// Merges files_[begin, end), which must all belong to one partition, into a single file
// that takes their place. Crash before the manifest is saved: the output is unlisted and
// deleted at startup. Crash after: the inputs are unlisted and deleted at startup.
Status Engine::CompactRun(size_t begin, size_t end) {
  std::vector<const BlockFileReader*> inputs;
  std::vector<uint64_t> series;
  for (size_t i = begin; i < end; ++i) {
    inputs.push_back(files_[i].reader.get());
    for (const BlockMeta& meta : files_[i].reader->blocks()) {
      series.push_back(meta.series_id);
    }
  }
  std::sort(series.begin(), series.end());
  series.erase(std::unique(series.begin(), series.end()), series.end());

  FileEntry output;
  output.number = next_file_number_++;
  output.partition = files_[begin].entry.partition;
  std::unique_ptr<BlockFileWriter> writer;
  Status s = BlockFileWriter::Create(BlockPath(output.number), &writer);
  if (!s.ok()) {
    return s;
  }
  // One series at a time, so memory use is bounded by the largest series in the partition.
  // The inputs are ordered oldest first, which is what makes the newest write win.
  Points points;
  for (const uint64_t series_id : series) {
    s = QueryRange(inputs, nullptr, series_id, TimeRange(), &points);
    if (s.ok()) {
      s = writer->AddSeries(series_id, points);
    }
    if (!s.ok()) {
      return s;
    }
  }
  s = writer->Finish();
  if (s.ok()) {
    s = FileSize(BlockPath(output.number), &output.size);
  }
  if (!s.ok()) {
    return s;
  }
  Hook("compact:output_written");

  std::vector<FileEntry> entries;
  for (size_t i = 0; i < files_.size(); ++i) {
    if (i == begin) {
      entries.push_back(output);
    }
    if (i < begin || i >= end) {
      entries.push_back(files_[i].entry);
    }
  }
  return Commit(std::move(entries), min_wal_segment_, "compact:manifest_saved");
}

size_t Engine::PartitionEnd(size_t begin) const {
  size_t end = begin + 1;
  while (end < files_.size() && files_[end].entry.partition == files_[begin].entry.partition) {
    ++end;
  }
  return end;
}

// Chooses what to merge among files_[begin, end), the files of one partition. Returns the
// index where the run to merge starts, or `end` if nothing should be merged.
//
// The run always ends at the newest file, so its output can take the run's place without
// changing which file is newer than which. Starting from the newest file, the run grows
// backwards while the next older file is no bigger than 1.2 times everything gathered so
// far. A file much bigger than the newer ones behind it stops the run, so big files are
// rewritten rarely and small ones often (the size-ratio rule of RocksDB's universal
// compaction).
size_t Engine::PickCompactionRun(size_t begin, size_t end) const {
  const size_t min_files = options_.compaction_min_files;
  const size_t count = end - begin;
  if (min_files < 2 || count < min_files) {
    return end;
  }
  // Safety valve: sizes that never line up must not let files pile up without limit.
  if (count >= 3 * min_files) {
    return begin;
  }
  size_t start = end - 1;
  uint64_t total = files_[start].entry.size;
  while (start > begin && files_[start - 1].entry.size * 5 <= total * 6) {
    --start;
    total += files_[start].entry.size;
  }
  return end - start >= min_files ? start : end;
}

Status Engine::CompactWherePolicySays() {
  // Every merge changes files_, so the scan starts over after each one. It ends because a
  // merge always leaves fewer files than before.
  size_t begin = 0;
  while (begin < files_.size()) {
    const size_t end = PartitionEnd(begin);
    const size_t start = PickCompactionRun(begin, end);
    if (start == end) {
      begin = end;
      continue;
    }
    Status s = CompactRun(start, end);
    if (!s.ok()) {
      return s;
    }
    begin = 0;
  }
  return Status::OK();
}

Status Engine::CompactEverything() {
  size_t begin = 0;
  while (begin < files_.size()) {
    const size_t end = PartitionEnd(begin);
    if (end - begin < 2) {
      begin = end;
      continue;
    }
    Status s = CompactRun(begin, end);
    if (!s.ok()) {
      return s;
    }
    begin = 0;
  }
  return Status::OK();
}

Status Engine::Guard(Status s) {
  if (!s.ok()) {
    fatal_ = s;
  }
  return s;
}

int64_t Engine::PartitionOf(int64_t timestamp_ns) const {
  return FloorDiv(timestamp_ns, options_.partition_duration_ns);
}

std::string Engine::ManifestPath() const { return dir_ + "/MANIFEST"; }

std::string Engine::SeriesLogPath() const { return dir_ + "/series.log"; }

std::string Engine::WalPath(uint64_t number) const {
  return dir_ + "/wal/" + NumberedName(number, kWalExtension);
}

std::string Engine::BlockPath(uint64_t number) const {
  return dir_ + "/blocks/" + NumberedName(number, kBlockExtension);
}

void Engine::Hook(std::string_view point) const {
  if (options_.test_hook) {
    options_.test_hook(point);
  }
}

}  // namespace tsengine
