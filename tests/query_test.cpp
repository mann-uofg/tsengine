#include "tsengine/query.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "test_util.h"
#include "tsengine/block_file.h"
#include "tsengine/memtable.h"
#include "tsengine/types.h"

namespace tsengine {
namespace {

class QueryTest : public TempDirTest {
 protected:
  // Flushes `memtable` to a new block file and adds it as the newest file.
  void FlushAsNewestFile(const Memtable& memtable) {
    const std::string path = Path("f" + std::to_string(files_.size()) + ".tsb");
    const Status s = FlushMemtable(memtable, path);
    ASSERT_TRUE(s.ok()) << s.ToString();
    std::unique_ptr<BlockFileReader> reader;
    ASSERT_TRUE(BlockFileReader::Open(path, &reader).ok());
    files_.push_back(reader.get());
    owned_.push_back(std::move(reader));
  }

  Points Query(const Memtable* memtable, uint64_t series_id, TimeRange range) {
    Points out;
    const Status s = QueryRange(files_, memtable, series_id, range, &out);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return out;
  }

  std::vector<std::unique_ptr<BlockFileReader>> owned_;
  std::vector<const BlockFileReader*> files_;
};

TEST_F(QueryTest, NoSourcesGivesNoPoints) {
  EXPECT_TRUE(Query(nullptr, 1, TimeRange()).empty());
  Memtable memtable;
  EXPECT_TRUE(Query(&memtable, 1, TimeRange()).empty());
}

TEST_F(QueryTest, CombinesFilesAndMemtableInTimeOrder) {
  Memtable first;
  first.Append(1, 10, 1.0);
  first.Append(1, 20, 2.0);
  ASSERT_NO_FATAL_FAILURE(FlushAsNewestFile(first));
  Memtable second;
  second.Append(1, 30, 3.0);
  second.Append(1, 40, 4.0);
  ASSERT_NO_FATAL_FAILURE(FlushAsNewestFile(second));
  Memtable live;
  live.Append(1, 50, 5.0);
  live.Append(1, 5, 0.5);

  const Points all = Query(&live, 1, TimeRange());
  EXPECT_EQ(all.timestamps, (std::vector<int64_t>{5, 10, 20, 30, 40, 50}));
  EXPECT_EQ(all.values, (std::vector<double>{0.5, 1.0, 2.0, 3.0, 4.0, 5.0}));

  const Points middle = Query(&live, 1, TimeRange{20, 40});
  EXPECT_EQ(middle.timestamps, (std::vector<int64_t>{20, 30, 40}));
  EXPECT_TRUE(Query(&live, 2, TimeRange()).empty());
}

TEST_F(QueryTest, NewestSourceWinsForARepeatedTimestamp) {
  Memtable oldest;
  oldest.Append(1, 10, 1.0);
  oldest.Append(1, 20, 1.0);
  oldest.Append(1, 30, 1.0);
  ASSERT_NO_FATAL_FAILURE(FlushAsNewestFile(oldest));
  EXPECT_EQ(Query(nullptr, 1, TimeRange()).values, (std::vector<double>{1.0, 1.0, 1.0}));

  Memtable newer;
  newer.Append(1, 20, 2.0);
  newer.Append(1, 30, 2.0);
  ASSERT_NO_FATAL_FAILURE(FlushAsNewestFile(newer));
  EXPECT_EQ(Query(nullptr, 1, TimeRange()).values, (std::vector<double>{1.0, 2.0, 2.0}));

  Memtable live;
  live.Append(1, 30, 3.0);
  const Points all = Query(&live, 1, TimeRange());
  EXPECT_EQ(all.timestamps, (std::vector<int64_t>{10, 20, 30}));
  EXPECT_EQ(all.values, (std::vector<double>{1.0, 2.0, 3.0}));
}

// Property test: random writes with heavy overlap, flushed at intervals, must read back the
// same as a std::map that simply keeps the last value written to each timestamp.
TEST_F(QueryTest, RandomWorkloadMatchesAModel) {
  std::mt19937_64 rng(2026);
  std::map<uint64_t, std::map<int64_t, double>> model;
  Memtable memtable;
  for (int op = 0; op < 6000; ++op) {
    const uint64_t id = 1 + rng() % 5;
    const auto t = static_cast<int64_t>(rng() % 3000);
    const double v = static_cast<double>(rng() % 1000) * 0.25;
    memtable.Append(id, t, v);
    model[id][t] = v;
    if (op % 1000 == 999 && op < 5000) {
      ASSERT_NO_FATAL_FAILURE(FlushAsNewestFile(memtable));
      memtable = Memtable();
    }
  }
  ASSERT_EQ(files_.size(), 5U);
  ASSERT_FALSE(memtable.empty());

  for (int q = 0; q < 500; ++q) {
    // Series 6 has no data at all.
    const uint64_t id = 1 + rng() % 6;
    const auto a = static_cast<int64_t>(rng() % 3200) - 100;
    const auto b = static_cast<int64_t>(rng() % 3200) - 100;
    TimeRange range{std::min(a, b), std::max(a, b)};
    if (q % 10 == 0) {
      range = TimeRange();
    }
    Points expected;
    for (const auto& [t, v] : model[id]) {
      if (range.Contains(t)) {
        expected.Append(t, v);
      }
    }
    ExpectSamePoints(Query(&memtable, id, range), expected);
  }
}

}  // namespace
}  // namespace tsengine
