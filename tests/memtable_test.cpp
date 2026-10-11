#include "tsengine/memtable.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "test_util.h"
#include "tsengine/block_file.h"
#include "tsengine/types.h"

namespace tsengine {
namespace {

Points Read(const Memtable& memtable, uint64_t series_id, TimeRange range) {
  Points out;
  memtable.ReadRange(series_id, range, &out);
  return out;
}

Points MakeExpected(std::vector<int64_t> timestamps, std::vector<double> values) {
  Points points;
  points.timestamps = std::move(timestamps);
  points.values = std::move(values);
  return points;
}

TEST(Memtable, ReturnsPointsInTheOrderAppended) {
  Memtable memtable;
  EXPECT_TRUE(memtable.empty());
  memtable.Append(1, 10, 1.0);
  memtable.Append(1, 20, 2.0);
  memtable.Append(1, 30, 3.0);
  EXPECT_FALSE(memtable.empty());
  EXPECT_EQ(memtable.point_count(), 3U);
  ExpectSamePoints(Read(memtable, 1, TimeRange()), MakeExpected({10, 20, 30}, {1.0, 2.0, 3.0}));
}

TEST(Memtable, SortsOutOfOrderPoints) {
  Memtable memtable;
  memtable.Append(1, 30, 3.0);
  memtable.Append(1, 10, 1.0);
  memtable.Append(1, 40, 4.0);
  memtable.Append(1, 20, 2.0);
  const Points expected = MakeExpected({10, 20, 30, 40}, {1.0, 2.0, 3.0, 4.0});
  ExpectSamePoints(Read(memtable, 1, TimeRange()), expected);
}

TEST(Memtable, LastWriteToATimestampWins) {
  Memtable memtable;
  memtable.Append(1, 10, 1.0);
  memtable.Append(1, 20, 2.0);
  memtable.Append(1, 10, 100.0);
  memtable.Append(1, 20, 200.0);
  memtable.Append(1, 20, 300.0);
  ExpectSamePoints(Read(memtable, 1, TimeRange()), MakeExpected({10, 20}, {100.0, 300.0}));
}

TEST(Memtable, RangeIncludesBothEnds) {
  Memtable ordered;
  Memtable shuffled;
  for (const int64_t t : {10, 20, 30, 40, 50}) {
    ordered.Append(1, t, static_cast<double>(t));
  }
  for (const int64_t t : {50, 10, 40, 20, 30}) {
    shuffled.Append(1, t, static_cast<double>(t));
  }
  const Points expected = MakeExpected({20, 30, 40}, {20.0, 30.0, 40.0});
  ExpectSamePoints(Read(ordered, 1, TimeRange{20, 40}), expected);
  ExpectSamePoints(Read(shuffled, 1, TimeRange{20, 40}), expected);
  ExpectSamePoints(Read(ordered, 1, TimeRange{15, 45}), expected);
  ExpectSamePoints(Read(shuffled, 1, TimeRange{15, 45}), expected);
  EXPECT_TRUE(Read(ordered, 1, TimeRange{21, 29}).empty());
  EXPECT_TRUE(Read(shuffled, 1, TimeRange{21, 29}).empty());
  EXPECT_TRUE(Read(ordered, 1, TimeRange{40, 20}).empty());
  EXPECT_TRUE(Read(shuffled, 1, TimeRange{40, 20}).empty());
}

TEST(Memtable, KeepsSeriesApart) {
  Memtable memtable;
  memtable.Append(9, 1, 9.0);
  memtable.Append(2, 1, 2.0);
  memtable.Append(5, 1, 5.0);
  memtable.Append(2, 2, 2.5);
  EXPECT_EQ(memtable.SeriesIds(), (std::vector<uint64_t>{2, 5, 9}));
  ExpectSamePoints(Read(memtable, 2, TimeRange()), MakeExpected({1, 2}, {2.0, 2.5}));
  ExpectSamePoints(Read(memtable, 5, TimeRange()), MakeExpected({1}, {5.0}));
  EXPECT_TRUE(Read(memtable, 7, TimeRange()).empty());
}

class MemtableFlushTest : public TempDirTest {};

TEST_F(MemtableFlushTest, FlushedFileHoldsWhatTheMemtableHeld) {
  Memtable memtable;
  // Series 3 arrives in order and is long enough to need several blocks.
  for (int64_t i = 0; i < 3000; ++i) {
    memtable.Append(3, i * 10, static_cast<double>(i % 50));
  }
  // Series 8 arrives backwards, with every timestamp written twice.
  for (int64_t i = 99; i >= 0; --i) {
    memtable.Append(8, i, -1.0);
    memtable.Append(8, i, static_cast<double>(i));
  }

  const Status s = FlushMemtable(memtable, Path("a.tsb"));
  ASSERT_TRUE(s.ok()) << s.ToString();
  std::unique_ptr<BlockFileReader> reader;
  ASSERT_TRUE(BlockFileReader::Open(Path("a.tsb"), &reader).ok());
  EXPECT_EQ(reader->blocks().size(), 4U);

  for (const uint64_t id : memtable.SeriesIds()) {
    Points from_file;
    ASSERT_TRUE(reader->ReadRange(id, TimeRange(), &from_file).ok());
    ExpectSamePoints(from_file, Read(memtable, id, TimeRange()));
  }
  Points series8;
  ASSERT_TRUE(reader->ReadRange(8, TimeRange{0, 2}, &series8).ok());
  ExpectSamePoints(series8, MakeExpected({0, 1, 2}, {0.0, 1.0, 2.0}));
}

TEST_F(MemtableFlushTest, FlushRefusesToOverwrite) {
  Memtable memtable;
  memtable.Append(1, 1, 1.0);
  ASSERT_TRUE(FlushMemtable(memtable, Path("a.tsb")).ok());
  EXPECT_EQ(FlushMemtable(memtable, Path("a.tsb")).code(), Status::Code::kInvalidArgument);
}

}  // namespace
}  // namespace tsengine
