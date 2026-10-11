#include "tsengine/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <cstdio>
#include <utility>
#include <vector>

#include "tsengine/coding.h"
#include "tsengine/crc32c.h"
#include "tsengine/file_util.h"

namespace tsengine {
namespace {

// A SeriesDef payload starts with series_id (u64) and key_len (u32).
constexpr size_t kSeriesDefFixedSize = 12;

// Cuts the file back to `valid_size`, dropping a torn record left by a crash.
Status TruncateTo(int fd, const std::string& path, uint64_t valid_size) {
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    return ErrnoStatus("fstat", path, errno);
  }
  const auto file_size = static_cast<uint64_t>(st.st_size);
  if (valid_size > file_size) {
    return Status::InvalidArgument(path + ": valid size is past the end of the file");
  }
  if (valid_size == file_size) {
    return Status::OK();
  }
  if (::ftruncate(fd, static_cast<off_t>(valid_size)) != 0) {
    return ErrnoStatus("ftruncate", path, errno);
  }
  return SyncFd(fd, path);
}

// Decodes one record whose checksum has already been verified and hands it to the handler.
// Anything wrong in here is corruption, not a torn write: the bytes are exactly what was
// written, and they still make no sense.
Status DispatchRecord(uint8_t type, std::string_view payload, WalReplayHandler* handler,
                      std::vector<Sample>* scratch) {
  if (type == static_cast<uint8_t>(WalRecordType::kSeriesDef)) {
    if (payload.size() < kSeriesDefFixedSize) {
      return Status::Corruption("series definition record is too short");
    }
    const uint64_t series_id = DecodeFixed64(payload.data());
    const uint32_t key_len = DecodeFixed32(payload.data() + 8);
    if (key_len != payload.size() - kSeriesDefFixedSize) {
      return Status::Corruption("series definition record has a bad key length");
    }
    handler->OnSeriesDef(series_id, payload.substr(kSeriesDefFixedSize));
    return Status::OK();
  }
  if (type == static_cast<uint8_t>(WalRecordType::kSamples)) {
    if (payload.size() % kWalSampleSize != 0) {
      return Status::Corruption("samples record length is not a multiple of the sample size");
    }
    scratch->clear();
    scratch->reserve(payload.size() / kWalSampleSize);
    for (size_t off = 0; off < payload.size(); off += kWalSampleSize) {
      const char* p = payload.data() + off;
      Sample sample;
      sample.series_id = DecodeFixed64(p);
      sample.timestamp_ns = static_cast<int64_t>(DecodeFixed64(p + 8));
      sample.value = std::bit_cast<double>(DecodeFixed64(p + 16));
      scratch->push_back(sample);
    }
    handler->OnSamples(*scratch);
    return Status::OK();
  }
  return Status::Corruption("unknown record type " + std::to_string(type));
}

}  // namespace

WalWriter::WalWriter(int fd, std::string path, uint64_t initial_size, const WalOptions& options)
    : fd_(fd), path_(std::move(path)), size_(initial_size), options_(options) {}

WalWriter::~WalWriter() { ::close(fd_); }

Status WalWriter::Create(const std::string& path, const WalOptions& options,
                         std::unique_ptr<WalWriter>* out) {
  if (::access(path.c_str(), F_OK) == 0) {
    return Status::InvalidArgument(path + ": segment already exists");
  }
  // Write the header to a temporary file and rename it into place. A file with a segment
  // name therefore always starts with a complete header, even if we crash in here.
  const std::string tmp_path = path + ".tmp";
  const int flags = O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC;
  const int fd = ::open(tmp_path.c_str(), flags, 0644);
  if (fd < 0) {
    return ErrnoStatus("open", tmp_path, errno);
  }
  char header[kWalSegmentHeaderSize] = {};
  EncodeFixed32(header, kWalMagic);
  EncodeFixed16(header + 4, kWalVersion);
  Status s = WriteAll(fd, tmp_path, header, sizeof(header));
  if (s.ok()) {
    s = SyncFd(fd, tmp_path);
  }
  if (s.ok() && std::rename(tmp_path.c_str(), path.c_str()) != 0) {
    s = ErrnoStatus("rename", tmp_path, errno);
  }
  if (s.ok()) {
    s = SyncParentDir(path);
  }
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  out->reset(new WalWriter(fd, path, kWalSegmentHeaderSize, options));
  return Status::OK();
}

Status WalWriter::OpenExisting(const std::string& path, uint64_t valid_size,
                               const WalOptions& options, std::unique_ptr<WalWriter>* out) {
  if (valid_size < kWalSegmentHeaderSize) {
    return Status::InvalidArgument(path + ": valid size is smaller than the segment header");
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0) {
    return ErrnoStatus("open", path, errno);
  }
  Status s = TruncateTo(fd, path, valid_size);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  out->reset(new WalWriter(fd, path, valid_size, options));
  return Status::OK();
}

Status WalWriter::AppendSeriesDef(uint64_t series_id, std::string_view key) {
  if (key.size() > kWalMaxPayloadSize - kSeriesDefFixedSize) {
    return Status::InvalidArgument("series key exceeds the maximum WAL record size");
  }
  buffer_.assign(kWalRecordHeaderSize, '\0');
  PutFixed64(&buffer_, series_id);
  PutFixed32(&buffer_, static_cast<uint32_t>(key.size()));
  buffer_.append(key);
  return WriteRecord(WalRecordType::kSeriesDef);
}

Status WalWriter::AppendSamples(std::span<const Sample> samples) {
  if (samples.empty()) {
    return Status::OK();
  }
  if (samples.size() > kWalMaxPayloadSize / kWalSampleSize) {
    return Status::InvalidArgument("sample batch exceeds the maximum WAL record size");
  }
  buffer_.assign(kWalRecordHeaderSize, '\0');
  buffer_.reserve(kWalRecordHeaderSize + samples.size() * kWalSampleSize);
  for (const Sample& sample : samples) {
    PutFixed64(&buffer_, sample.series_id);
    PutFixed64(&buffer_, static_cast<uint64_t>(sample.timestamp_ns));
    PutFixed64(&buffer_, std::bit_cast<uint64_t>(sample.value));
  }
  return WriteRecord(WalRecordType::kSamples);
}

Status WalWriter::WriteRecord(WalRecordType type) {
  if (failed_) {
    return Status::IOError(path_ + ": writer is unusable after an earlier I/O error");
  }
  const size_t payload_size = buffer_.size() - kWalRecordHeaderSize;
  EncodeFixed32(buffer_.data() + 4, static_cast<uint32_t>(payload_size));
  buffer_[8] = static_cast<char>(type);
  EncodeFixed32(buffer_.data(), Crc32c(buffer_.data() + 4, buffer_.size() - 4));

  // One write call for the whole record keeps the window for a torn record as small as the
  // kernel allows.
  Status s = WriteAll(fd_, path_, buffer_.data(), buffer_.size());
  if (s.ok() && options_.sync_on_append) {
    s = SyncFd(fd_, path_);
  }
  if (!s.ok()) {
    failed_ = true;
    return s;
  }
  size_ += buffer_.size();
  return Status::OK();
}

Status WalWriter::Sync() {
  if (failed_) {
    return Status::IOError(path_ + ": writer is unusable after an earlier I/O error");
  }
  Status s = SyncFd(fd_, path_);
  if (!s.ok()) {
    failed_ = true;
  }
  return s;
}

Status ReplayWalSegment(const std::string& path, WalReplayHandler* handler,
                        WalReplayResult* result) {
  *result = WalReplayResult();
  std::string data;
  Status s = ReadFile(path, &data);
  if (!s.ok()) {
    return s;
  }
  if (data.size() < kWalSegmentHeaderSize) {
    return Status::Corruption(path + ": segment header is missing");
  }
  if (DecodeFixed32(data.data()) != kWalMagic) {
    return Status::Corruption(path + ": not a WAL segment (bad magic)");
  }
  if (DecodeFixed16(data.data() + 4) != kWalVersion) {
    return Status::Corruption(path + ": unsupported WAL version");
  }

  std::vector<Sample> scratch;
  size_t pos = kWalSegmentHeaderSize;
  result->valid_size = pos;
  while (pos < data.size()) {
    const size_t remaining = data.size() - pos;
    const char* header = data.data() + pos;
    if (remaining < kWalRecordHeaderSize) {
      result->tail_reason = "incomplete record header";
      break;
    }
    const uint32_t stored_crc = DecodeFixed32(header);
    const uint32_t length = DecodeFixed32(header + 4);
    // Check the length before trusting it: a garbage value must not make us read past the
    // end of the file or allocate gigabytes.
    if (length > kWalMaxPayloadSize) {
      result->tail_reason = "record length exceeds the maximum";
      break;
    }
    if (length > remaining - kWalRecordHeaderSize) {
      result->tail_reason = "incomplete record payload";
      break;
    }
    if (Crc32c(header + 4, kWalRecordHeaderSize - 4 + length) != stored_crc) {
      result->tail_reason = "checksum mismatch";
      break;
    }

    const auto type = static_cast<uint8_t>(header[8]);
    const std::string_view payload(header + kWalRecordHeaderSize, length);
    s = DispatchRecord(type, payload, handler, &scratch);
    if (!s.ok()) {
      return Status::Corruption(path + ": " + s.message());
    }
    pos += kWalRecordHeaderSize + length;
    result->valid_size = pos;
    ++result->records;
  }
  result->torn_tail = result->valid_size != data.size();
  return Status::OK();
}

}  // namespace tsengine
