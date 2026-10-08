#pragma once

#include <string>
#include <utility>

namespace tsengine {

// Result of an operation that can fail. The engine does not throw on I/O or bad input: every
// fallible function returns a Status, and the compiler rejects code that ignores one.
class [[nodiscard]] Status {
 public:
  enum class Code { kOk, kInvalidArgument, kIOError, kCorruption };

  Status() = default;

  static Status OK() { return Status(); }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }
  static Status IOError(std::string msg) { return Status(Code::kIOError, std::move(msg)); }
  static Status Corruption(std::string msg) { return Status(Code::kCorruption, std::move(msg)); }

  bool ok() const { return code_ == Code::kOk; }
  Code code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const {
    switch (code_) {
      case Code::kOk:
        return "OK";
      case Code::kInvalidArgument:
        return "Invalid argument: " + message_;
      case Code::kIOError:
        return "IO error: " + message_;
      case Code::kCorruption:
        return "Corruption: " + message_;
    }
    return "Unknown";
  }

 private:
  Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}

  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace tsengine
