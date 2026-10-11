#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "test_util.h"
#include "tsengine/engine.h"
#include "tsengine/query.h"
#include "tsengine/types.h"

namespace tsengine {
namespace {

// Aggregates one series through the engine and compares the buckets with ones computed the
// slow way from a std::map of everything written. Values are multiples of 0.25, so sums are
// exact whatever order they are added in.
class AggregateTest : public TempDirTest {
 protected:
  void SetUp() override {
    TempDirTest::SetUp();
    // One huge partition and no automatic flush or compaction: each test lays the data out
    // in files exactly as it wants.
    options_.partition_duration_ns = 1000000000;
    options_.memtable_flush_points = 100000000;
    options_.compaction_min_files = 0;
    options_.sync_writes = false;
  }

  void Open() {
    engine_.reset();
    const Status s = Engine::Open(Path("db"), options_, &engine_);
    ASSERT_TRUE(s.ok()) << s.ToString();
    ASSERT_TRUE(engine_->GetOrCreateSeries("m value", &id_).ok());
  }

  void Write(int64_t timestamp, double value) {
    const Sample sample{id_, timestamp, value};
    ASSERT_TRUE(engine_->Write(std::span<const Sample>(&sample, 1)).ok());
    model_[timestamp] = value;
  }

  // Writes `count` points at timestamps 0, 1, 2, ... in order.
  void WriteSequence(int64_t count) {
    for (int64_t t = 0; t < count; ++t) {
      ASSERT_NO_FATAL_FAILURE(Write(t, static_cast<double>(t % 37) * 0.25));
    }
  }

  AggregateResult Run(TimeRange range, int64_t bucket_width) {
    AggregateRequest request;
    request.series_id = id_;
    request.range = range;
    request.bucket_width = bucket_width;
    AggregateResult result;
    const Status s = engine_->Aggregate(request, &result);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return result;
  }

  std::vector<Bucket> Expected(TimeRange range, int64_t bucket_width) {
    std::vector<Bucket> buckets;
    for (const auto& [timestamp, value] : model_) {
      if (!range.Contains(timestamp)) {
        continue;
      }
      int64_t start = range.min;
      if (bucket_width > 0) {
        start = FloorDiv(timestamp, bucket_width) * bucket_width;
      }
      if (buckets.empty() || buckets.back().start != start) {
        Bucket bucket;
        bucket.start = start;
        buckets.push_back(bucket);
      }
      buckets.back().Add(value);
    }
    return buckets;
  }

  // Runs the query, checks the buckets against the model and returns how it was answered.
  AggregateStats Check(TimeRange range, int64_t bucket_width) {
    const AggregateResult result = Run(range, bucket_width);
    const std::vector<Bucket> expected = Expected(range, bucket_width);
    EXPECT_EQ(result.buckets.size(), expected.size());
    for (size_t i = 0; i < std::min(result.buckets.size(), expected.size()); ++i) {
      EXPECT_EQ(result.buckets[i].start, expected[i].start) << "bucket " << i;
      EXPECT_EQ(result.buckets[i].count, expected[i].count) << "bucket " << i;
      EXPECT_EQ(result.buckets[i].sum, expected[i].sum) << "bucket " << i;
      EXPECT_EQ(result.buckets[i].min, expected[i].min) << "bucket " << i;
      EXPECT_EQ(result.buckets[i].max, expected[i].max) << "bucket " << i;
    }
    return result.stats;
  }

  // Checks `count` random ranges and bucket widths. Returns how many blocks were answered
  // from statistics in total.
  uint64_t CheckRandomQueries(std::mt19937_64* rng, int count) {
    const std::vector<int64_t> widths = {0, 1, 7, 64, 100, 500, 2000, 5000, 100000};
    uint64_t from_stats = 0;
    for (int q = 0; q < count; ++q) {
      const auto a = static_cast<int64_t>((*rng)() % 12000) - 2000;
      const auto b = static_cast<int64_t>((*rng)() % 12000) - 2000;
      TimeRange range{std::min(a, b), std::max(a, b)};
      if (q % 8 == 0) {
        range = TimeRange();
      }
      from_stats += Check(range, widths[(*rng)() % widths.size()]).blocks_from_stats;
    }
    return from_stats;
  }

  EngineOptions options_;
  std::unique_ptr<Engine> engine_;
  uint64_t id_ = 0;
  std::map<int64_t, double> model_;
};

TEST_F(AggregateTest, NoPointsMeansNoBuckets) {
  ASSERT_NO_FATAL_FAILURE(Open());
  EXPECT_TRUE(Run(TimeRange(), 0).buckets.empty());
  ASSERT_NO_FATAL_FAILURE(Write(10, 1.0));
  EXPECT_TRUE(Run(TimeRange{20, 30}, 5).buckets.empty());
  EXPECT_TRUE(Run(TimeRange{30, 20}, 5).buckets.empty());
}

TEST_F(AggregateTest, PointsInTheMemtableAreBucketed) {
  ASSERT_NO_FATAL_FAILURE(Open());
  for (const int64_t t : {-15, -1, 0, 4, 5, 9, 10, 27}) {
    ASSERT_NO_FATAL_FAILURE(Write(t, static_cast<double>(t) * 0.5));
  }
  const AggregateStats stats = Check(TimeRange(), 10);
  EXPECT_EQ(stats.blocks_from_stats, 0U);
  EXPECT_EQ(stats.blocks_decoded, 0U);
  // Buckets start at multiples of the width, also below zero.
  const AggregateResult result = Run(TimeRange(), 10);
  ASSERT_EQ(result.buckets.size(), 5U);
  EXPECT_EQ(result.buckets[0].start, -20);
  EXPECT_EQ(result.buckets[1].start, -10);
  EXPECT_EQ(result.buckets[2].start, 0);
  EXPECT_EQ(result.buckets[2].count, 4U);
  Check(TimeRange{-1, 9}, 10);
  Check(TimeRange{4, 10}, 3);
}

// 5000 points in order make five blocks: four of 1024 points and one of 904.
TEST_F(AggregateTest, WholeBlocksAreAnsweredFromTheirStatistics) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_NO_FATAL_FAILURE(WriteSequence(5000));
  ASSERT_TRUE(engine_->Flush().ok());

  // One bucket for everything: no block needs to be read at all.
  AggregateStats stats = Check(TimeRange(), 0);
  EXPECT_EQ(stats.blocks_from_stats, 5U);
  EXPECT_EQ(stats.blocks_decoded, 0U);

  // Buckets that line up with the blocks.
  stats = Check(TimeRange(), 1024);
  EXPECT_EQ(stats.blocks_from_stats, 5U);
  EXPECT_EQ(stats.blocks_decoded, 0U);

  // Buckets wider than a block.
  stats = Check(TimeRange(), 4096);
  EXPECT_EQ(stats.blocks_from_stats, 5U);
  EXPECT_EQ(stats.blocks_decoded, 0U);
}

TEST_F(AggregateTest, BlocksCutByTheRangeAreDecoded) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_NO_FATAL_FAILURE(WriteSequence(5000));
  ASSERT_TRUE(engine_->Flush().ok());

  AggregateStats stats = Check(TimeRange{10, 4999}, 0);
  EXPECT_EQ(stats.blocks_from_stats, 4U);
  EXPECT_EQ(stats.blocks_decoded, 1U);

  stats = Check(TimeRange{0, 4990}, 0);
  EXPECT_EQ(stats.blocks_from_stats, 4U);
  EXPECT_EQ(stats.blocks_decoded, 1U);

  stats = Check(TimeRange{1500, 3500}, 0);
  EXPECT_EQ(stats.blocks_from_stats, 1U);
  EXPECT_EQ(stats.blocks_decoded, 2U);
}

TEST_F(AggregateTest, BlocksSpanningSeveralBucketsAreDecoded) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_NO_FATAL_FAILURE(WriteSequence(5000));
  ASSERT_TRUE(engine_->Flush().ok());

  AggregateStats stats = Check(TimeRange(), 100);
  EXPECT_EQ(stats.blocks_from_stats, 0U);
  EXPECT_EQ(stats.blocks_decoded, 5U);

  // Buckets of 1000 do not line up with blocks of 1024: the first four blocks each straddle
  // a boundary. Only the last block, 4096 to 4999, sits inside one bucket.
  stats = Check(TimeRange(), 1000);
  EXPECT_EQ(stats.blocks_from_stats, 1U);
  EXPECT_EQ(stats.blocks_decoded, 4U);
}

TEST_F(AggregateTest, SmallerBlocksFitNarrowerBuckets) {
  options_.block_points = 100;
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_NO_FATAL_FAILURE(WriteSequence(5000));
  ASSERT_TRUE(engine_->Flush().ok());

  AggregateStats stats = Check(TimeRange(), 500);
  EXPECT_EQ(stats.blocks_from_stats, 50U);
  EXPECT_EQ(stats.blocks_decoded, 0U);

  stats = Check(TimeRange{250, 4749}, 500);
  EXPECT_EQ(stats.blocks_from_stats, 44U);
  EXPECT_EQ(stats.blocks_decoded, 2U);
}

// A newer file rewrites one point. The statistics of the old block that held it are now
// wrong, so that block must be decoded together with the newer one.
TEST_F(AggregateTest, OverwrittenBlockIsDecodedNotTrusted) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_NO_FATAL_FAILURE(WriteSequence(5000));
  ASSERT_TRUE(engine_->Flush().ok());
  ASSERT_NO_FATAL_FAILURE(Write(2000, 999.75));
  ASSERT_TRUE(engine_->Flush().ok());

  const AggregateStats stats = Check(TimeRange(), 0);
  EXPECT_EQ(stats.blocks_from_stats, 4U);
  EXPECT_EQ(stats.blocks_decoded, 2U);
  EXPECT_EQ(Run(TimeRange(), 0).buckets[0].max, 999.75);
  EXPECT_EQ(Run(TimeRange(), 0).buckets[0].count, 5000U);
}

TEST_F(AggregateTest, MemtableOverlapForcesADecode) {
  ASSERT_NO_FATAL_FAILURE(Open());
  ASSERT_NO_FATAL_FAILURE(WriteSequence(5000));
  ASSERT_TRUE(engine_->Flush().ok());

  // Newer points beyond the flushed data do not touch any block.
  ASSERT_NO_FATAL_FAILURE(Write(6000, 1.25));
  ASSERT_NO_FATAL_FAILURE(Write(6001, 2.25));
  AggregateStats stats = Check(TimeRange(), 0);
  EXPECT_EQ(stats.blocks_from_stats, 5U);
  EXPECT_EQ(stats.blocks_decoded, 0U);

  // A late point inside the first block's span does. The memtable now covers 100 to 6001,
  // which overlaps every block.
  ASSERT_NO_FATAL_FAILURE(Write(100, -3.5));
  stats = Check(TimeRange(), 0);
  EXPECT_EQ(stats.blocks_from_stats, 0U);
  EXPECT_EQ(stats.blocks_decoded, 5U);
  EXPECT_EQ(Run(TimeRange(), 0).buckets[0].min, -3.5);
}

TEST_F(AggregateTest, SurvivesCompactionAndRestart) {
  ASSERT_NO_FATAL_FAILURE(Open());
  for (int round = 0; round < 4; ++round) {
    for (int64_t t = round; t < 3000; t += 4) {
      ASSERT_NO_FATAL_FAILURE(Write(t, static_cast<double>((t + round) % 50) * 0.25));
    }
    ASSERT_TRUE(engine_->Flush().ok());
  }
  // Four files whose blocks interleave: nothing can be answered from statistics.
  AggregateStats stats = Check(TimeRange(), 0);
  EXPECT_EQ(stats.blocks_from_stats, 0U);

  // After compaction there is one file with no overlaps, so everything can.
  ASSERT_TRUE(engine_->Compact().ok());
  stats = Check(TimeRange(), 0);
  EXPECT_EQ(stats.blocks_decoded, 0U);
  EXPECT_EQ(stats.blocks_from_stats, 3U);

  ASSERT_NO_FATAL_FAILURE(Open());
  Check(TimeRange(), 0);
  Check(TimeRange{17, 2900}, 250);
}

// Property test: random writes with overwrites across several partitions and files, then
// many random ranges and bucket widths.
TEST_F(AggregateTest, RandomQueriesMatchAModel) {
  options_.partition_duration_ns = 2000;
  options_.block_points = 64;
  ASSERT_NO_FATAL_FAILURE(Open());
  std::mt19937_64 rng(11);
  for (int op = 0; op < 8000; ++op) {
    const auto t = static_cast<int64_t>(rng() % 10000) - 1000;
    ASSERT_NO_FATAL_FAILURE(Write(t, static_cast<double>(rng() % 400) * 0.25));
    if (op % 1500 == 1499) {
      ASSERT_TRUE(engine_->Flush().ok());
    }
    if (op == 5000) {
      ASSERT_TRUE(engine_->Compact().ok());
    }
  }

  // First with the data as the writes left it: several overlapping files per partition and
  // a memtable that covers most of the time range.
  CheckRandomQueries(&rng, 200);

  // Then tidied up: one file per partition, and a memtable that only holds newer points.
  // Now many blocks qualify for push-down, and the answers must still be the same.
  ASSERT_TRUE(engine_->Flush().ok());
  ASSERT_TRUE(engine_->Compact().ok());
  for (int64_t t = 9500; t < 9550; ++t) {
    ASSERT_NO_FATAL_FAILURE(Write(t, static_cast<double>(rng() % 400) * 0.25));
  }
  // The comparison is only a test of push-down if push-down actually happened.
  EXPECT_GT(CheckRandomQueries(&rng, 400), 0U);
}

}  // namespace
}  // namespace tsengine
