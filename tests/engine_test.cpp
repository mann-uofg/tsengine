#include "tsengine/engine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "test_util.h"
#include "tsengine/types.h"

namespace tsengine {
namespace {

// Every test writes through Write(), which also records the point in a std::map. The map is
// the model: whatever the engine has been through, a query must return what the map holds.
class EngineTest : public TempDirTest {
 protected:
  void SetUp() override {
    TempDirTest::SetUp();
    // Small partitions and no automatic flush or compaction, so each test decides exactly
    // when those happen. fsync is off to keep the tests fast.
    options_.partition_duration_ns = 1000;
    options_.memtable_flush_points = 1000000;
    options_.compaction_min_files = 0;
    options_.sync_writes = false;
  }

  // Opens the database, or closes and reopens it as a restarted process would.
  void Reopen() {
    engine_.reset();
    const Status s = Engine::Open(Path("db"), options_, &engine_);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  uint64_t Series(const std::string& key) {
    uint64_t id = 0;
    const Status s = engine_->GetOrCreateSeries(key, &id);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return id;
  }

  void Write(uint64_t series_id, int64_t timestamp, double value) {
    const Sample sample{series_id, timestamp, value};
    const Status s = engine_->Write(std::span<const Sample>(&sample, 1));
    ASSERT_TRUE(s.ok()) << s.ToString();
    model_[series_id][timestamp] = value;
  }

  Points Expected(uint64_t series_id, TimeRange range) {
    Points expected;
    for (const auto& [timestamp, value] : model_[series_id]) {
      if (range.Contains(timestamp)) {
        expected.Append(timestamp, value);
      }
    }
    return expected;
  }

  void CheckRange(uint64_t series_id, TimeRange range) {
    Points got;
    const Status s = engine_->Query(series_id, range, &got);
    ASSERT_TRUE(s.ok()) << s.ToString();
    ExpectSamePoints(got, Expected(series_id, range));
  }

  void CheckAgainstModel() {
    for (const auto& entry : model_) {
      ASSERT_NO_FATAL_FAILURE(CheckRange(entry.first, TimeRange()));
    }
  }

  size_t FilesOnDisk(const std::string& subdir) {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(Path("db/" + subdir))) {
      if (entry.is_regular_file()) {
        ++count;
      }
    }
    return count;
  }

  EngineOptions options_;
  std::unique_ptr<Engine> engine_;
  std::map<uint64_t, std::map<int64_t, double>> model_;
};

TEST_F(EngineTest, ReadsBackWhatWasWritten) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t cpu = Series("cpu,host=a usage");
  const uint64_t mem = Series("mem,host=a used");
  EXPECT_NE(cpu, mem);
  ASSERT_NO_FATAL_FAILURE(Write(cpu, 10, 1.0));
  ASSERT_NO_FATAL_FAILURE(Write(cpu, 30, 3.0));
  ASSERT_NO_FATAL_FAILURE(Write(cpu, 20, 2.0));
  ASSERT_NO_FATAL_FAILURE(Write(mem, 10, 512.0));
  CheckAgainstModel();
  CheckRange(cpu, TimeRange{15, 30});
  CheckRange(cpu, TimeRange{31, 99});
  CheckRange(12345, TimeRange());
}

TEST_F(EngineTest, SeriesIdsAreStable) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t a = Series("a");
  const uint64_t b = Series("b");
  EXPECT_EQ(Series("a"), a);
  EXPECT_EQ(engine_->series_count(), 2U);
  uint64_t found = 0;
  EXPECT_TRUE(engine_->FindSeries("b", &found));
  EXPECT_EQ(found, b);
  EXPECT_FALSE(engine_->FindSeries("c", &found));

  ASSERT_NO_FATAL_FAILURE(Reopen());
  EXPECT_EQ(engine_->series_count(), 2U);
  EXPECT_EQ(Series("a"), a);
  EXPECT_EQ(Series("b"), b);
  const uint64_t c = Series("c");
  EXPECT_NE(c, a);
  EXPECT_NE(c, b);
}

// Nothing has been flushed, so after a restart the data can only come from the WAL.
TEST_F(EngineTest, UnflushedWritesSurviveARestart) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  for (int64_t t = 0; t < 200; ++t) {
    ASSERT_NO_FATAL_FAILURE(Write(id, t * 7, static_cast<double>(t)));
  }
  EXPECT_TRUE(engine_->Files().empty());

  ASSERT_NO_FATAL_FAILURE(Reopen());
  EXPECT_TRUE(engine_->Files().empty());
  EXPECT_EQ(engine_->memtable_points(), 200U);
  CheckAgainstModel();
}

TEST_F(EngineTest, FlushMovesPointsFromTheWalToABlockFile) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  for (int64_t t = 0; t < 100; ++t) {
    ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(t)));
  }
  EXPECT_EQ(engine_->wal_segment(), 1U);
  ASSERT_TRUE(engine_->Flush().ok());

  EXPECT_EQ(engine_->memtable_points(), 0U);
  ASSERT_EQ(engine_->Files().size(), 1U);
  EXPECT_EQ(engine_->Files()[0].partition, 0);
  EXPECT_GT(engine_->Files()[0].size, 0U);
  // The segment that held those points is gone and a fresh one has taken over.
  EXPECT_EQ(engine_->wal_segment(), 2U);
  EXPECT_FALSE(std::filesystem::exists(Path("db/wal/00000001.wal")));
  EXPECT_TRUE(std::filesystem::exists(Path("db/wal/00000002.wal")));
  CheckAgainstModel();

  ASSERT_NO_FATAL_FAILURE(Reopen());
  EXPECT_EQ(engine_->memtable_points(), 0U);
  EXPECT_EQ(engine_->Files().size(), 1U);
  CheckAgainstModel();
}

TEST_F(EngineTest, FlushingAnEmptyMemtableDoesNothing) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  ASSERT_TRUE(engine_->Flush().ok());
  EXPECT_TRUE(engine_->Files().empty());
  EXPECT_EQ(engine_->wal_segment(), 1U);
}

TEST_F(EngineTest, FlushesByItselfWhenTheMemtableIsFull) {
  options_.memtable_flush_points = 50;
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  for (int64_t t = 0; t < 120; ++t) {
    ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(t)));
  }
  EXPECT_EQ(engine_->Files().size(), 2U);
  EXPECT_EQ(engine_->memtable_points(), 20U);
  CheckAgainstModel();
}

TEST_F(EngineTest, EachPartitionGetsItsOwnFile) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t a = Series("a");
  const uint64_t b = Series("b");
  // Partitions are 1000 wide: these land in partitions -1, 0 and 2.
  for (const int64_t t : {-500, -1, 0, 999, 2000, 2500}) {
    ASSERT_NO_FATAL_FAILURE(Write(a, t, static_cast<double>(t)));
  }
  ASSERT_NO_FATAL_FAILURE(Write(b, 2999, 9.0));
  ASSERT_TRUE(engine_->Flush().ok());

  const std::vector<FileEntry> files = engine_->Files();
  ASSERT_EQ(files.size(), 3U);
  EXPECT_EQ(files[0].partition, -1);
  EXPECT_EQ(files[1].partition, 0);
  EXPECT_EQ(files[2].partition, 2);
  CheckAgainstModel();
  CheckRange(a, TimeRange{-1, 0});
  CheckRange(a, TimeRange{1000, 1999});
  CheckRange(a, TimeRange{999, 2000});
}

TEST_F(EngineTest, NewerFlushWinsForARepeatedTimestamp) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  for (int round = 0; round < 3; ++round) {
    for (int64_t t = 0; t < 50; ++t) {
      ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(round * 100 + t)));
    }
    ASSERT_TRUE(engine_->Flush().ok());
  }
  ASSERT_NO_FATAL_FAILURE(Write(id, 7, -1.0));
  EXPECT_EQ(engine_->Files().size(), 3U);
  CheckAgainstModel();
  ASSERT_NO_FATAL_FAILURE(Reopen());
  CheckAgainstModel();
}

TEST_F(EngineTest, CompactMergesEachPartitionIntoOneFile) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t a = Series("a");
  const uint64_t b = Series("b");
  std::mt19937 rng(5);
  for (int round = 0; round < 6; ++round) {
    for (int i = 0; i < 80; ++i) {
      const uint64_t id = rng() % 2 == 0 ? a : b;
      const auto t = static_cast<int64_t>(rng() % 3000);
      ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(rng() % 100)));
    }
    ASSERT_TRUE(engine_->Flush().ok());
  }
  EXPECT_EQ(engine_->Files().size(), 18U);

  ASSERT_TRUE(engine_->Compact().ok());
  const std::vector<FileEntry> files = engine_->Files();
  ASSERT_EQ(files.size(), 3U);
  EXPECT_EQ(files[0].partition, 0);
  EXPECT_EQ(files[1].partition, 1);
  EXPECT_EQ(files[2].partition, 2);
  // The inputs are deleted, not just forgotten.
  EXPECT_EQ(FilesOnDisk("blocks"), 3U);
  CheckAgainstModel();

  ASSERT_NO_FATAL_FAILURE(Reopen());
  EXPECT_EQ(engine_->Files().size(), 3U);
  CheckAgainstModel();
}

TEST_F(EngineTest, AutomaticCompactionKeepsTheFileCountBounded) {
  options_.compaction_min_files = 4;
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  std::mt19937 rng(6);
  size_t most_files = 0;
  for (int round = 0; round < 60; ++round) {
    for (int i = 0; i < 40; ++i) {
      const auto t = static_cast<int64_t>(rng() % 1000);
      ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(rng() % 100)));
    }
    ASSERT_TRUE(engine_->Flush().ok());
    most_files = std::max(most_files, engine_->Files().size());
  }
  // 60 flushes into one partition. Without compaction there would be 60 files.
  EXPECT_LT(most_files, 12U);
  EXPECT_EQ(FilesOnDisk("blocks"), engine_->Files().size());
  CheckAgainstModel();
}

TEST_F(EngineTest, RetentionDropsWholePartitions) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  for (int64_t t = 0; t < 5000; t += 50) {
    ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(t)));
  }
  ASSERT_TRUE(engine_->Flush().ok());
  ASSERT_EQ(engine_->Files().size(), 5U);

  // 2500 is inside partition 2, so partitions 0 and 1 go and partition 2 stays whole.
  ASSERT_TRUE(engine_->DropPartitionsBefore(2500).ok());
  model_[id].erase(model_[id].begin(), model_[id].lower_bound(2000));
  ASSERT_EQ(engine_->Files().size(), 3U);
  EXPECT_EQ(engine_->Files()[0].partition, 2);
  EXPECT_EQ(FilesOnDisk("blocks"), 3U);
  CheckAgainstModel();

  ASSERT_NO_FATAL_FAILURE(Reopen());
  EXPECT_EQ(engine_->Files().size(), 3U);
  CheckAgainstModel();

  // Nothing older is left, so a second pass changes nothing.
  ASSERT_TRUE(engine_->DropPartitionsBefore(2500).ok());
  EXPECT_EQ(engine_->Files().size(), 3U);
}

// What a crash in the middle of an append leaves behind: half a record at the end of the
// newest WAL segment.
TEST_F(EngineTest, TornWalTailIsCutOffAndWritingContinues) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  for (int64_t t = 0; t < 20; ++t) {
    ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(t)));
  }
  engine_.reset();
  const std::string segment = Path("db/wal/00000001.wal");
  WriteFileBytes(segment, ReadFileBytes(segment) + std::string("\x13\x37\x00\x42\x99", 5));

  ASSERT_NO_FATAL_FAILURE(Reopen());
  CheckAgainstModel();
  // Points written after the recovery must still be there after the next one.
  for (int64_t t = 20; t < 40; ++t) {
    ASSERT_NO_FATAL_FAILURE(Write(id, t, static_cast<double>(t)));
  }
  ASSERT_NO_FATAL_FAILURE(Reopen());
  EXPECT_EQ(engine_->memtable_points(), 40U);
  CheckAgainstModel();
}

// Files a crashed flush or compaction can leave behind.
TEST_F(EngineTest, UnlistedFilesAreRemovedOnOpen) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  const uint64_t id = Series("s");
  ASSERT_NO_FATAL_FAILURE(Write(id, 1, 1.0));
  ASSERT_TRUE(engine_->Flush().ok());
  ASSERT_NO_FATAL_FAILURE(Write(id, 2, 2.0));
  engine_.reset();
  WriteFileBytes(Path("db/blocks/00000777.tsb"), "a block file nobody committed");
  WriteFileBytes(Path("db/blocks/00000778.tsb.tmp"), "half a block file");
  WriteFileBytes(Path("db/wal/00000009.wal.tmp"), "half a segment header");

  ASSERT_NO_FATAL_FAILURE(Reopen());
  EXPECT_FALSE(std::filesystem::exists(Path("db/blocks/00000777.tsb")));
  EXPECT_FALSE(std::filesystem::exists(Path("db/blocks/00000778.tsb.tmp")));
  EXPECT_FALSE(std::filesystem::exists(Path("db/wal/00000009.wal.tmp")));
  EXPECT_EQ(FilesOnDisk("blocks"), 1U);
  CheckAgainstModel();
}

TEST_F(EngineTest, RefusesADifferentPartitionSize) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  engine_.reset();
  options_.partition_duration_ns = 2000;
  const Status s = Engine::Open(Path("db"), options_, &engine_);
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(EngineTest, DamagedManifestStopsTheDatabaseFromOpening) {
  ASSERT_NO_FATAL_FAILURE(Reopen());
  engine_.reset();
  std::string bytes = ReadFileBytes(Path("db/MANIFEST"));
  bytes[10] = static_cast<char>(bytes[10] ^ 0x01);
  WriteFileBytes(Path("db/MANIFEST"), bytes);
  const Status s = Engine::Open(Path("db"), options_, &engine_);
  EXPECT_EQ(s.code(), Status::Code::kCorruption);
}

// Property test: a long random mix of writes, flushes, compactions, retention and restarts,
// with heavy overwriting. The engine must agree with the model throughout.
TEST_F(EngineTest, RandomOperationsMatchAModel) {
  options_.memtable_flush_points = 300;
  options_.compaction_min_files = 3;
  ASSERT_NO_FATAL_FAILURE(Reopen());
  std::vector<uint64_t> ids;
  for (int i = 0; i < 6; ++i) {
    ids.push_back(Series("series-" + std::to_string(i)));
  }

  std::mt19937_64 rng(42);
  int64_t horizon = 0;
  for (int op = 0; op < 6000; ++op) {
    const uint64_t roll = rng() % 1000;
    if (roll < 4) {
      ASSERT_NO_FATAL_FAILURE(Reopen());
    } else if (roll < 8) {
      ASSERT_TRUE(engine_->Flush().ok());
    } else if (roll < 10) {
      ASSERT_TRUE(engine_->Compact().ok());
    } else if (roll < 11) {
      // Retention only touches flushed data, so flush first to keep the model exact.
      ASSERT_TRUE(engine_->Flush().ok());
      horizon += 1000;
      ASSERT_TRUE(engine_->DropPartitionsBefore(horizon).ok());
      for (auto& entry : model_) {
        entry.second.erase(entry.second.begin(), entry.second.lower_bound(horizon));
      }
    } else {
      // Mostly recent timestamps, with some arriving late but never before the horizon.
      const auto t = horizon + static_cast<int64_t>(rng() % 8000);
      const double v = static_cast<double>(rng() % 1000) * 0.5;
      ASSERT_NO_FATAL_FAILURE(Write(ids[rng() % ids.size()], t, v));
    }
    if (op % 500 == 499) {
      ASSERT_NO_FATAL_FAILURE(CheckAgainstModel());
    }
  }

  ASSERT_NO_FATAL_FAILURE(CheckAgainstModel());
  ASSERT_NO_FATAL_FAILURE(Reopen());
  ASSERT_NO_FATAL_FAILURE(CheckAgainstModel());
  for (int q = 0; q < 300; ++q) {
    const auto a = horizon + static_cast<int64_t>(rng() % 9000) - 500;
    const auto b = horizon + static_cast<int64_t>(rng() % 9000) - 500;
    const TimeRange range{std::min(a, b), std::max(a, b)};
    ASSERT_NO_FATAL_FAILURE(CheckRange(ids[rng() % ids.size()], range));
  }
  EXPECT_EQ(FilesOnDisk("blocks"), engine_->Files().size());
}

}  // namespace
}  // namespace tsengine
