#include "tsengine/manifest.h"

#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>

#include "tsengine/coding.h"
#include "tsengine/crc32c.h"
#include "tsengine/file_util.h"

namespace tsengine {
namespace {

constexpr size_t kHeaderSize = 40;
constexpr size_t kEntrySize = 24;
constexpr size_t kChecksumSize = 4;

}  // namespace

Status SaveManifest(const std::string& path, const Manifest& manifest) {
  std::string data(8, '\0');
  EncodeFixed32(data.data(), kManifestMagic);
  EncodeFixed16(data.data() + 4, kManifestVersion);
  PutFixed64(&data, static_cast<uint64_t>(manifest.partition_duration_ns));
  PutFixed64(&data, manifest.next_file_number);
  PutFixed64(&data, manifest.min_wal_segment);
  PutFixed64(&data, manifest.files.size());
  for (const FileEntry& file : manifest.files) {
    PutFixed64(&data, file.number);
    PutFixed64(&data, static_cast<uint64_t>(file.partition));
    PutFixed64(&data, file.size);
  }
  const uint32_t crc = Crc32c(data.data(), data.size());
  PutFixed32(&data, crc);
  return WriteFileAtomic(path, data);
}

Status LoadManifest(const std::string& path, Manifest* manifest, bool* found) {
  *found = false;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) {
      return Status::IOError("stat " + path + ": " + ec.message());
    }
    return Status::OK();
  }
  std::string data;
  Status s = ReadFile(path, &data);
  if (!s.ok()) {
    return s;
  }
  if (data.size() < kHeaderSize + kChecksumSize) {
    return Status::Corruption(path + ": manifest is too short");
  }
  const char* p = data.data();
  const size_t body_size = data.size() - kChecksumSize;
  if (Crc32c(p, body_size) != DecodeFixed32(p + body_size)) {
    return Status::Corruption(path + ": manifest checksum mismatch");
  }
  if (DecodeFixed32(p) != kManifestMagic) {
    return Status::Corruption(path + ": not a manifest (bad magic)");
  }
  if (DecodeFixed16(p + 4) != kManifestVersion) {
    return Status::Corruption(path + ": unsupported manifest version");
  }

  Manifest loaded;
  loaded.partition_duration_ns = static_cast<int64_t>(DecodeFixed64(p + 8));
  loaded.next_file_number = DecodeFixed64(p + 16);
  loaded.min_wal_segment = DecodeFixed64(p + 24);
  const uint64_t count = DecodeFixed64(p + 32);
  const size_t entries_size = body_size - kHeaderSize;
  if (count > entries_size || entries_size != count * kEntrySize) {
    return Status::Corruption(path + ": manifest size does not match its file count");
  }
  if (loaded.partition_duration_ns <= 0) {
    return Status::Corruption(path + ": manifest has an invalid partition duration");
  }
  loaded.files.reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    const char* e = p + kHeaderSize + i * kEntrySize;
    FileEntry file;
    file.number = DecodeFixed64(e);
    file.partition = static_cast<int64_t>(DecodeFixed64(e + 8));
    file.size = DecodeFixed64(e + 16);
    if (!loaded.files.empty() && loaded.files.back().partition > file.partition) {
      return Status::Corruption(path + ": manifest files are not ordered by partition");
    }
    loaded.files.push_back(file);
  }
  *manifest = std::move(loaded);
  *found = true;
  return Status::OK();
}

}  // namespace tsengine
