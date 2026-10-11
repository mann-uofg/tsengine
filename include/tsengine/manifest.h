#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tsengine/status.h"

namespace tsengine {

// The manifest is the one file that says which other files make up the database. A block
// file that is not listed here does not exist as far as the engine is concerned, however
// complete it looks on disk. Every flush, compaction and retention pass ends by replacing
// the manifest in a single atomic step, and that replacement is the moment the change
// takes effect.
//
// Layout (all integers little-endian):
//
//   magic u32 "TSMF" | version u16 | reserved u16
//   partition duration i64 | next file number u64 | min WAL segment u64 | file count u64
//   per file: number u64 | partition i64 | size u64
//   crc32c u32 of everything above

inline constexpr uint32_t kManifestMagic = 0x464D5354U;  // the bytes "TSMF" on disk
inline constexpr uint16_t kManifestVersion = 1;

// One live block file.
struct FileEntry {
  // The file is blocks/<number>.tsb.
  uint64_t number = 0;
  // Index of the time partition the file belongs to: floor(timestamp / partition duration).
  int64_t partition = 0;
  // Size in bytes, used to choose which files to compact together.
  uint64_t size = 0;

  bool operator==(const FileEntry&) const = default;
};

struct Manifest {
  int64_t partition_duration_ns = 0;
  // The number the next new block file will get.
  uint64_t next_file_number = 1;
  // WAL segments with a lower number have been flushed and are no longer needed.
  uint64_t min_wal_segment = 1;
  // Ordered by partition, and oldest first within a partition.
  std::vector<FileEntry> files;

  bool operator==(const Manifest&) const = default;
};

// Atomically replaces the manifest at `path`.
Status SaveManifest(const std::string& path, const Manifest& manifest);

// Reads the manifest at `path`. If there is no such file, returns OK with *found == false.
Status LoadManifest(const std::string& path, Manifest* manifest, bool* found);

}  // namespace tsengine
