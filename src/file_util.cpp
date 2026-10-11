#include "tsengine/file_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace tsengine {

Status ErrnoStatus(std::string_view op, const std::string& path, int err) {
  std::string msg(op);
  msg += " ";
  msg += path;
  msg += ": ";
  msg += std::generic_category().message(err);
  return Status::IOError(std::move(msg));
}

Status WriteAll(int fd, const std::string& path, const char* data, size_t n) {
  while (n > 0) {
    const ssize_t written = ::write(fd, data, n);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ErrnoStatus("write", path, errno);
    }
    data += written;
    n -= static_cast<size_t>(written);
  }
  return Status::OK();
}

Status SyncFd(int fd, const std::string& path) {
  if (::fdatasync(fd) != 0) {
    return ErrnoStatus("fdatasync", path, errno);
  }
  return Status::OK();
}

Status SyncParentDir(const std::string& path) {
  std::string dir = std::filesystem::path(path).parent_path().string();
  if (dir.empty()) {
    dir = ".";
  }
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return ErrnoStatus("open", dir, errno);
  }
  Status s = Status::OK();
  if (::fsync(fd) != 0) {
    s = ErrnoStatus("fsync", dir, errno);
  }
  ::close(fd);
  return s;
}

Status ReadFile(const std::string& path, std::string* out) {
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
  out->resize(static_cast<size_t>(st.st_size));
  size_t done = 0;
  Status s = Status::OK();
  while (done < out->size()) {
    const ssize_t n = ::read(fd, out->data() + done, out->size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      s = ErrnoStatus("read", path, errno);
      break;
    }
    if (n == 0) {
      break;
    }
    done += static_cast<size_t>(n);
  }
  ::close(fd);
  out->resize(done);
  return s;
}

Status WriteFileAtomic(const std::string& path, std::string_view data) {
  const std::string tmp_path = path + ".tmp";
  const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return ErrnoStatus("open", tmp_path, errno);
  }
  Status s = WriteAll(fd, tmp_path, data.data(), data.size());
  if (s.ok()) {
    s = SyncFd(fd, tmp_path);
  }
  ::close(fd);
  if (s.ok() && std::rename(tmp_path.c_str(), path.c_str()) != 0) {
    s = ErrnoStatus("rename", tmp_path, errno);
  }
  if (s.ok()) {
    s = SyncParentDir(path);
  }
  return s;
}

}  // namespace tsengine
