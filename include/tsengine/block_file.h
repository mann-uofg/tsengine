#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tsengine/status.h"
#include "tsengine/types.h"

namespace tsengine {

// An immutable file of compressed blocks, written once by a memtable flush. Layout (all
// integers little-endian), see docs/design.md:
//
//   header:  magic u32 "TSBK" | version u16 | reserved u16
//   blocks:  for each block, its timestamp stream followed by its value stream
//   index:   one 72-byte entry per block, sorted by (series id, time)
//   footer:  index offset u64 | block count u64 | index crc32c u32 | magic u32
//
// A block holds up to kMaxBlockPoints points of one series. Its index entry carries the
// time range and summary statistics, so a query can skip or answer from a block without
// decoding it.

inline constexpr uint32_t kBlockFileMagic = 0x4B425354U;  // the bytes "TSBK" on disk
inline constexpr uint16_t kBlockFileVersion = 1;
inline constexpr size_t kBlockFileHeaderSize = 8;
inline constexpr size_t kBlockFileFooterSize = 24;
inline constexpr size_t kBlockIndexEntrySize = 72;
inline constexpr size_t kMaxBlockPoints = 1024;

// Index entry of one block.
struct BlockMeta {
  uint64_t series_id = 0;
  int64_t min_timestamp = 0;
  int64_t max_timestamp = 0;
  // Where the block starts in the file, and how long it is.
  uint64_t offset = 0;
  uint32_t size = 0;
  // How many of those bytes are the timestamp stream; the rest is the value stream.
  uint32_t timestamp_size = 0;
  uint32_t count = 0;
  // CRC32C of the block's bytes.
  uint32_t crc = 0;
  // Summary of the block's values.
  double sum = 0.0;
  double min = 0.0;
  double max = 0.0;
};

// Builds a block file. The file is written under a temporary name and only gets its real
// name in Finish(), after it is complete and synced, so readers never see a partial file.
class BlockFileWriter {
 public:
  // Fails if `path` already exists.
  static Status Create(const std::string& path, std::unique_ptr<BlockFileWriter>* out);

  // Removes the temporary file if Finish() was never reached.
  ~BlockFileWriter();
  BlockFileWriter(const BlockFileWriter&) = delete;
  BlockFileWriter& operator=(const BlockFileWriter&) = delete;

  // Adds all points of one series. Timestamps must be strictly increasing, and series must
  // be added in increasing id order. The points are split into blocks as needed.
  Status AddSeries(uint64_t series_id, const Points& points);

  // Writes the index and footer, syncs the file and renames it into place.
  Status Finish();

 private:
  BlockFileWriter(int fd, std::string path);

  Status FlushBuffer();

  int fd_;
  std::string path_;
  std::string tmp_path_;
  // Bytes not yet written to the file, and the file offset just past them.
  std::string buffer_;
  uint64_t offset_ = 0;
  // Scratch space for the block being encoded.
  std::string block_;
  std::vector<BlockMeta> index_;
  bool has_series_ = false;
  uint64_t last_series_id_ = 0;
  bool finished_ = false;
  bool failed_ = false;
};

// Reads a block file through a memory mapping. Safe to use from several threads at once:
// nothing changes after Open().
class BlockFileReader {
 public:
  // Maps the file and validates its header, footer and index.
  static Status Open(const std::string& path, std::unique_ptr<BlockFileReader>* out);

  ~BlockFileReader();
  BlockFileReader(const BlockFileReader&) = delete;
  BlockFileReader& operator=(const BlockFileReader&) = delete;

  // Index entries of every block, sorted by series id and then by time.
  const std::vector<BlockMeta>& blocks() const { return blocks_; }

  // Verifies and decodes one block into `out`, replacing its contents. `meta` must be an
  // element of blocks().
  Status ReadBlock(const BlockMeta& meta, Points* out) const;

  // Appends to `out` every point of `series_id` inside `range`, in time order. Only blocks
  // whose time range overlaps `range` are decoded.
  Status ReadRange(uint64_t series_id, TimeRange range, Points* out) const;

 private:
  BlockFileReader(std::string path, void* map, size_t size);

  Status LoadIndex();

  std::string path_;
  void* map_;
  size_t size_;
  std::vector<BlockMeta> blocks_;
};

}  // namespace tsengine
