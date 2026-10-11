#include "tsengine/block_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <span>
#include <string_view>
#include <utility>

#include "tsengine/codec.h"
#include "tsengine/coding.h"
#include "tsengine/crc32c.h"
#include "tsengine/file_util.h"

namespace tsengine {
namespace {

// Encoded blocks are collected in memory and written out in chunks of about this size.
constexpr size_t kWriteBufferSize = 1 << 20;

void EncodeIndexEntry(const BlockMeta& meta, std::string* out) {
  PutFixed64(out, meta.series_id);
  PutFixed64(out, static_cast<uint64_t>(meta.min_timestamp));
  PutFixed64(out, static_cast<uint64_t>(meta.max_timestamp));
  PutFixed64(out, meta.offset);
  PutFixed32(out, meta.size);
  PutFixed32(out, meta.timestamp_size);
  PutFixed32(out, meta.count);
  PutFixed32(out, meta.crc);
  PutFixed64(out, std::bit_cast<uint64_t>(meta.sum));
  PutFixed64(out, std::bit_cast<uint64_t>(meta.min));
  PutFixed64(out, std::bit_cast<uint64_t>(meta.max));
}

BlockMeta DecodeIndexEntry(const char* p) {
  BlockMeta meta;
  meta.series_id = DecodeFixed64(p);
  meta.min_timestamp = static_cast<int64_t>(DecodeFixed64(p + 8));
  meta.max_timestamp = static_cast<int64_t>(DecodeFixed64(p + 16));
  meta.offset = DecodeFixed64(p + 24);
  meta.size = DecodeFixed32(p + 32);
  meta.timestamp_size = DecodeFixed32(p + 36);
  meta.count = DecodeFixed32(p + 40);
  meta.crc = DecodeFixed32(p + 44);
  meta.sum = std::bit_cast<double>(DecodeFixed64(p + 48));
  meta.min = std::bit_cast<double>(DecodeFixed64(p + 56));
  meta.max = std::bit_cast<double>(DecodeFixed64(p + 64));
  return meta;
}

Status Corrupt(const std::string& path, std::string_view why) {
  std::string msg = path;
  msg += ": ";
  msg += why;
  return Status::Corruption(std::move(msg));
}

// True if block `a` sorts entirely before block `b` in the index.
bool ComesBefore(const BlockMeta& a, const BlockMeta& b) {
  if (a.series_id != b.series_id) {
    return a.series_id < b.series_id;
  }
  return a.max_timestamp < b.min_timestamp;
}

// True if every point of `block` sorts before the point (series_id, timestamp).
bool EndsBefore(const BlockMeta& block, uint64_t series_id, int64_t timestamp) {
  if (block.series_id != series_id) {
    return block.series_id < series_id;
  }
  return block.max_timestamp < timestamp;
}

}  // namespace

BlockFileWriter::BlockFileWriter(int fd, std::string path)
    : fd_(fd), path_(std::move(path)), tmp_path_(path_ + ".tmp") {
  char header[kBlockFileHeaderSize] = {};
  EncodeFixed32(header, kBlockFileMagic);
  EncodeFixed16(header + 4, kBlockFileVersion);
  buffer_.assign(header, sizeof(header));
  offset_ = kBlockFileHeaderSize;
}

BlockFileWriter::~BlockFileWriter() {
  ::close(fd_);
  if (!finished_) {
    ::unlink(tmp_path_.c_str());
  }
}

Status BlockFileWriter::Create(const std::string& path, std::unique_ptr<BlockFileWriter>* out) {
  if (::access(path.c_str(), F_OK) == 0) {
    return Status::InvalidArgument(path + ": file already exists");
  }
  const std::string tmp_path = path + ".tmp";
  const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return ErrnoStatus("open", tmp_path, errno);
  }
  out->reset(new BlockFileWriter(fd, path));
  return Status::OK();
}

Status BlockFileWriter::AddSeries(uint64_t series_id, const Points& points) {
  if (finished_ || failed_) {
    return Status::InvalidArgument(path_ + ": writer is finished or has failed");
  }
  if (points.timestamps.size() != points.values.size()) {
    return Status::InvalidArgument("timestamp and value counts differ");
  }
  if (has_series_ && series_id <= last_series_id_) {
    return Status::InvalidArgument("series must be added in increasing id order");
  }
  for (size_t i = 1; i < points.timestamps.size(); ++i) {
    if (points.timestamps[i] <= points.timestamps[i - 1]) {
      return Status::InvalidArgument("timestamps must be strictly increasing");
    }
  }
  if (points.empty()) {
    return Status::OK();
  }
  has_series_ = true;
  last_series_id_ = series_id;

  const std::span<const int64_t> all_timestamps(points.timestamps);
  const std::span<const double> all_values(points.values);
  for (size_t begin = 0; begin < all_timestamps.size(); begin += kMaxBlockPoints) {
    const size_t n = std::min(kMaxBlockPoints, all_timestamps.size() - begin);
    const std::span<const int64_t> timestamps = all_timestamps.subspan(begin, n);
    const std::span<const double> values = all_values.subspan(begin, n);

    BlockMeta meta;
    block_.clear();
    EncodeTimestamps(timestamps, &block_);
    meta.timestamp_size = static_cast<uint32_t>(block_.size());
    EncodeValues(values, &block_);
    meta.series_id = series_id;
    meta.min_timestamp = timestamps.front();
    meta.max_timestamp = timestamps.back();
    meta.offset = offset_;
    meta.size = static_cast<uint32_t>(block_.size());
    meta.count = static_cast<uint32_t>(n);
    meta.crc = Crc32c(block_.data(), block_.size());
    meta.min = values.front();
    meta.max = values.front();
    for (const double v : values) {
      meta.sum += v;
      meta.min = std::min(meta.min, v);
      meta.max = std::max(meta.max, v);
    }
    index_.push_back(meta);

    buffer_ += block_;
    offset_ += block_.size();
    if (buffer_.size() >= kWriteBufferSize) {
      Status s = FlushBuffer();
      if (!s.ok()) {
        return s;
      }
    }
  }
  return Status::OK();
}

Status BlockFileWriter::FlushBuffer() {
  Status s = WriteAll(fd_, tmp_path_, buffer_.data(), buffer_.size());
  buffer_.clear();
  if (!s.ok()) {
    failed_ = true;
  }
  return s;
}

Status BlockFileWriter::Finish() {
  if (finished_ || failed_) {
    return Status::InvalidArgument(path_ + ": writer is finished or has failed");
  }
  std::string index_bytes;
  index_bytes.reserve(index_.size() * kBlockIndexEntrySize);
  for (const BlockMeta& meta : index_) {
    EncodeIndexEntry(meta, &index_bytes);
  }
  buffer_ += index_bytes;
  PutFixed64(&buffer_, offset_);
  PutFixed64(&buffer_, index_.size());
  PutFixed32(&buffer_, Crc32c(index_bytes.data(), index_bytes.size()));
  PutFixed32(&buffer_, kBlockFileMagic);

  // Same sequence as a new WAL segment: the file gets its real name only once it is
  // complete and on disk, and the rename itself is then made durable.
  Status s = FlushBuffer();
  if (s.ok()) {
    s = SyncFd(fd_, tmp_path_);
  }
  if (s.ok() && std::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
    s = ErrnoStatus("rename", tmp_path_, errno);
  }
  if (s.ok()) {
    s = SyncParentDir(path_);
  }
  if (!s.ok()) {
    failed_ = true;
    return s;
  }
  finished_ = true;
  return Status::OK();
}

BlockFileReader::BlockFileReader(std::string path, void* map, size_t size)
    : path_(std::move(path)), map_(map), size_(size) {}

BlockFileReader::~BlockFileReader() { ::munmap(map_, size_); }

Status BlockFileReader::Open(const std::string& path, std::unique_ptr<BlockFileReader>* out) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return ErrnoStatus("open", path, errno);
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    Status s = ErrnoStatus("fstat", path, errno);
    ::close(fd);
    return s;
  }
  const auto size = static_cast<size_t>(st.st_size);
  if (size < kBlockFileHeaderSize + kBlockFileFooterSize) {
    ::close(fd);
    return Corrupt(path, "file is too small to be a block file");
  }
  void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  const int mmap_errno = errno;
  // The mapping stays valid after the descriptor is closed.
  ::close(fd);
  if (map == MAP_FAILED) {
    return ErrnoStatus("mmap", path, mmap_errno);
  }
  std::unique_ptr<BlockFileReader> reader(new BlockFileReader(path, map, size));
  Status s = reader->LoadIndex();
  if (!s.ok()) {
    return s;
  }
  *out = std::move(reader);
  return Status::OK();
}

Status BlockFileReader::LoadIndex() {
  const char* data = static_cast<const char*>(map_);
  if (DecodeFixed32(data) != kBlockFileMagic) {
    return Corrupt(path_, "not a block file (bad magic)");
  }
  if (DecodeFixed16(data + 4) != kBlockFileVersion) {
    return Corrupt(path_, "unsupported block file version");
  }
  const char* footer = data + size_ - kBlockFileFooterSize;
  if (DecodeFixed32(footer + 20) != kBlockFileMagic) {
    return Corrupt(path_, "footer is missing; the file is incomplete");
  }
  const uint64_t index_offset = DecodeFixed64(footer);
  const uint64_t block_count = DecodeFixed64(footer + 8);
  const uint32_t index_crc = DecodeFixed32(footer + 16);
  const uint64_t index_end = size_ - kBlockFileFooterSize;
  if (index_offset < kBlockFileHeaderSize || index_offset > index_end) {
    return Corrupt(path_, "index offset is out of range");
  }
  const uint64_t index_size = index_end - index_offset;
  if (block_count > index_size || index_size != block_count * kBlockIndexEntrySize) {
    return Corrupt(path_, "index size does not match the block count");
  }
  const char* index_data = data + index_offset;
  if (Crc32c(index_data, index_size) != index_crc) {
    return Corrupt(path_, "index checksum mismatch");
  }

  // The checksum only proves the index is what was written. Still check that every entry
  // points inside the file, so a bad entry can never send a read out of bounds.
  blocks_.reserve(block_count);
  for (uint64_t i = 0; i < block_count; ++i) {
    const BlockMeta meta = DecodeIndexEntry(index_data + i * kBlockIndexEntrySize);
    if (meta.offset < kBlockFileHeaderSize || meta.offset > index_offset) {
      return Corrupt(path_, "block offset is out of range");
    }
    if (meta.size > index_offset - meta.offset || meta.timestamp_size > meta.size) {
      return Corrupt(path_, "block size is out of range");
    }
    if (meta.count == 0 || meta.count > kMaxBlockPoints) {
      return Corrupt(path_, "block point count is out of range");
    }
    if (meta.min_timestamp > meta.max_timestamp) {
      return Corrupt(path_, "block time range is inverted");
    }
    if (!blocks_.empty() && !ComesBefore(blocks_.back(), meta)) {
      return Corrupt(path_, "index is not sorted");
    }
    blocks_.push_back(meta);
  }
  return Status::OK();
}

Status BlockFileReader::ReadBlock(const BlockMeta& meta, Points* out) const {
  const char* block = static_cast<const char*>(map_) + meta.offset;
  if (Crc32c(block, meta.size) != meta.crc) {
    return Corrupt(path_, "block checksum mismatch");
  }
  const std::string_view timestamps(block, meta.timestamp_size);
  const std::string_view values(block + meta.timestamp_size, meta.size - meta.timestamp_size);
  Status s = DecodeTimestamps(timestamps, meta.count, &out->timestamps);
  if (s.ok()) {
    s = DecodeValues(values, meta.count, &out->values);
  }
  if (!s.ok()) {
    return Corrupt(path_, s.message());
  }
  return Status::OK();
}

Status BlockFileReader::ReadRange(uint64_t series_id, TimeRange range, Points* out) const {
  if (range.min > range.max) {
    return Status::OK();
  }
  // Binary search for the first block that could hold a point at or after range.min.
  size_t lo = 0;
  size_t hi = blocks_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (EndsBefore(blocks_[mid], series_id, range.min)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  Points block;
  for (size_t i = lo; i < blocks_.size(); ++i) {
    const BlockMeta& meta = blocks_[i];
    if (meta.series_id != series_id || meta.min_timestamp > range.max) {
      break;
    }
    Status s = ReadBlock(meta, &block);
    if (!s.ok()) {
      return s;
    }
    AppendRange(block, range, out);
  }
  return Status::OK();
}

}  // namespace tsengine
