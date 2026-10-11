#include "tsengine/manifest.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>

#include "test_util.h"

namespace tsengine {
namespace {

class ManifestTest : public TempDirTest {};

Manifest SampleManifest() {
  Manifest manifest;
  manifest.partition_duration_ns = 1000;
  manifest.next_file_number = 12;
  manifest.min_wal_segment = 4;
  manifest.files.push_back(FileEntry{3, -2, 500});
  manifest.files.push_back(FileEntry{7, 0, 4096});
  manifest.files.push_back(FileEntry{11, 0, 128});
  manifest.files.push_back(FileEntry{9, 5, 77});
  return manifest;
}

TEST_F(ManifestTest, MissingFileIsNotAnError) {
  Manifest manifest;
  bool found = true;
  ASSERT_TRUE(LoadManifest(Path("MANIFEST"), &manifest, &found).ok());
  EXPECT_FALSE(found);
}

TEST_F(ManifestTest, RoundTrips) {
  const Manifest saved = SampleManifest();
  ASSERT_TRUE(SaveManifest(Path("MANIFEST"), saved).ok());
  Manifest loaded;
  bool found = false;
  const Status s = LoadManifest(Path("MANIFEST"), &loaded, &found);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_TRUE(found);
  EXPECT_TRUE(loaded == saved);
}

TEST_F(ManifestTest, SavingAgainReplacesTheOldContents) {
  Manifest manifest = SampleManifest();
  ASSERT_TRUE(SaveManifest(Path("MANIFEST"), manifest).ok());
  manifest.files.clear();
  manifest.next_file_number = 99;
  ASSERT_TRUE(SaveManifest(Path("MANIFEST"), manifest).ok());

  Manifest loaded;
  bool found = false;
  ASSERT_TRUE(LoadManifest(Path("MANIFEST"), &loaded, &found).ok());
  EXPECT_TRUE(loaded == manifest);
  EXPECT_FALSE(std::filesystem::exists(Path("MANIFEST.tmp")));
}

TEST_F(ManifestTest, AnyDamagedByteIsDetected) {
  ASSERT_TRUE(SaveManifest(Path("MANIFEST"), SampleManifest()).ok());
  const std::string bytes = ReadFileBytes(Path("MANIFEST"));
  Manifest loaded;
  bool found = false;
  for (size_t pos = 0; pos < bytes.size(); ++pos) {
    std::string damaged = bytes;
    damaged[pos] = static_cast<char>(damaged[pos] ^ 0x20);
    WriteFileBytes(Path("MANIFEST"), damaged);
    const Status s = LoadManifest(Path("MANIFEST"), &loaded, &found);
    EXPECT_EQ(s.code(), Status::Code::kCorruption) << "pos=" << pos;
  }
}

TEST_F(ManifestTest, TruncatedFileIsDetected) {
  ASSERT_TRUE(SaveManifest(Path("MANIFEST"), SampleManifest()).ok());
  const std::string bytes = ReadFileBytes(Path("MANIFEST"));
  Manifest loaded;
  bool found = false;
  for (size_t len = 0; len < bytes.size(); ++len) {
    WriteFileBytes(Path("MANIFEST"), bytes.substr(0, len));
    const Status s = LoadManifest(Path("MANIFEST"), &loaded, &found);
    EXPECT_EQ(s.code(), Status::Code::kCorruption) << "len=" << len;
  }
}

}  // namespace
}  // namespace tsengine
