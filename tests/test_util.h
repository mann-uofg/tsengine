#pragma once

#include <gtest/gtest.h>
#include <stdlib.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "tsengine/types.h"

namespace tsengine {

// Fixture that gives each test its own empty directory and removes it afterwards.
class TempDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    std::string dir = (base / "tsengine_test_XXXXXX").string();
    ASSERT_NE(::mkdtemp(dir.data()), nullptr);
    dir_ = dir;
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string Path(const std::string& name) const { return dir_ + "/" + name; }

  std::string dir_;
};

inline std::string ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void WriteFileBytes(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// `n` points starting at `start` and `step` apart, with values that repeat every 17 points.
inline Points MakePoints(int64_t start, int64_t step, size_t n) {
  Points points;
  for (size_t i = 0; i < n; ++i) {
    points.Append(start + step * static_cast<int64_t>(i), static_cast<double>(i % 17) * 0.5);
  }
  return points;
}

// The points of `all` inside `range`, found the slow and obvious way.
inline Points Slice(const Points& all, TimeRange range) {
  Points out;
  for (size_t i = 0; i < all.size(); ++i) {
    if (range.Contains(all.timestamps[i])) {
      out.Append(all.timestamps[i], all.values[i]);
    }
  }
  return out;
}

inline void ExpectSamePoints(const Points& actual, const Points& expected) {
  EXPECT_EQ(actual.timestamps, expected.timestamps);
  EXPECT_EQ(actual.values, expected.values);
}

}  // namespace tsengine
