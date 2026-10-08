#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "tsengine/status.h"

namespace tsengine {

// Write-ahead log. Format (all integers little-endian), see docs/design.md:
//
//   segment header:  magic u32 "TSWL" | version u16 | reserved u16
//   record:          crc32c u32 | length u32 | type u8 | payload
//
// The CRC covers length, type and payload.

struct Sample {
  uint64_t series_id = 0;
  int64_t timestamp_ns = 0;
  double value = 0.0;
};

inline constexpr uint32_t kWalMagic = 0x4C575354U;  // the bytes "TSWL" on disk
inline constexpr uint16_t kWalVersion = 1;
inline constexpr size_t kWalSegmentHeaderSize = 8;
inline constexpr size_t kWalRecordHeaderSize = 9;
inline constexpr size_t kWalSampleSize = 24;
inline constexpr uint32_t kWalMaxPayloadSize = 16U << 20;

enum class WalRecordType : uint8_t { kSeriesDef = 1, kSamples = 2 };

struct WalOptions {
  // If true, every Append call fsyncs before it returns, so a successful return means the
  // record is durable. If false, the caller decides when to call Sync() (group commit or a
  // timer) and must not acknowledge a write before then.
  bool sync_on_append = true;
};

// Appends records to one segment file. Not thread-safe: the engine has a single writer.
class WalWriter {
 public:
  // Creates a new segment. Fails if `path` already exists.
  static Status Create(const std::string& path, const WalOptions& options,
                       std::unique_ptr<WalWriter>* out);

  // Reopens a segment after replay. The file is first cut back to `valid_size` (as reported
  // by ReplayWalSegment), which removes a torn record left by a crash.
  static Status OpenExisting(const std::string& path, uint64_t valid_size,
                             const WalOptions& options, std::unique_ptr<WalWriter>* out);

  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Records that `series_id` refers to the series with this key.
  Status AppendSeriesDef(uint64_t series_id, std::string_view key);

  // Appends one batch of points as a single record. The batch is all-or-nothing on replay.
  Status AppendSamples(std::span<const Sample> samples);

  // Forces everything appended so far to stable storage.
  Status Sync();

  // Size of the segment file in bytes, counting only complete records.
  uint64_t size() const { return size_; }

 private:
  WalWriter(int fd, std::string path, uint64_t initial_size, const WalOptions& options);

  // Fills in the header of the record held in buffer_ and writes it to the file.
  Status WriteRecord(WalRecordType type);

  int fd_;
  std::string path_;
  uint64_t size_;
  WalOptions options_;
  // Set after a failed write or fsync. The file may then end in a partial record, and a
  // failed fsync cannot be retried safely, so the writer refuses all further work.
  bool failed_ = false;
  std::string buffer_;
};

// Receives the records of a segment in the order they were written.
class WalReplayHandler {
 public:
  virtual ~WalReplayHandler() = default;
  virtual void OnSeriesDef(uint64_t series_id, std::string_view key) = 0;
  virtual void OnSamples(std::span<const Sample> samples) = 0;
};

struct WalReplayResult {
  // Offset just past the last good record. Appending must resume here.
  uint64_t valid_size = 0;
  // Number of records delivered to the handler.
  uint64_t records = 0;
  // True if there are bytes after valid_size that do not form a valid record.
  bool torn_tail = false;
  // Why reading stopped, when torn_tail is true.
  std::string tail_reason;
};

// Reads a segment front to back and hands each record to `handler`.
//
// Reading stops at the first record that is incomplete or fails its checksum. That is not an
// error here: the function returns OK with torn_tail set, and the caller decides what it
// means. In the newest segment it is a crash mid-append; in an older one it is corruption.
//
// Returns Corruption if the segment header is missing or wrong, or if a record passes its
// checksum but its contents make no sense.
Status ReplayWalSegment(const std::string& path, WalReplayHandler* handler,
                        WalReplayResult* result);

}  // namespace tsengine
