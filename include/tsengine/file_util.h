#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "tsengine/status.h"

namespace tsengine {

// Small wrappers over POSIX file calls that report failures as a Status.

// Builds an IOError like "open /path/file: No such file or directory" from an errno value.
Status ErrnoStatus(std::string_view op, const std::string& path, int err);

// write(2) may write fewer bytes than asked or be interrupted by a signal; this loops until
// everything is written.
Status WriteAll(int fd, const std::string& path, const char* data, size_t n);

// Forces the file's data to stable storage.
Status SyncFd(int fd, const std::string& path);

// A new or renamed file is only durable once its directory entry is, so this fsyncs the
// directory that contains `path`.
Status SyncParentDir(const std::string& path);

// Reads a whole file into memory.
Status ReadFile(const std::string& path, std::string* out);

}  // namespace tsengine
