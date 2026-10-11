#include "tsengine/block_file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_util.h"
#include "tsengine/types.h"

namespace tsengine {
namespace {

class BlockFileTest : public TempDirTest {};

void WriteOneSeries(const std::string& path, uint64_t series_id, const Points& points) {
  std::unique_ptr<BlockFileWriter> writer;
  ASSERT_TRUE(BlockFileWriter::Create(path, &writer).ok());
  ASSERT_TRUE(writer->AddSeries(series_id, points).ok());
  ASSERT_TRUE(writer->Finish().ok());
}

// Reads `range` of series `id` and compares it with the same slice of `all`.
void CheckRange(const BlockFileReader& file, uint64_t id, const Points& all, TimeRange range) {
  Points got;
  const Status s = file.ReadRange(id, range, &got);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ExpectSamePoints(got, Slice(all, range));
}

TEST_F(BlockFileTest, RoundTripsOneSeries) {
  const Points points = MakePoints(1000, 10, 10);
  ASSERT_NO_FATAL_FAILURE(WriteOneSeries(Path("a.tsb"), 42, points));

  std::unique_ptr<BlockFileReader> reader;
  const Status s = BlockFileReader::Open(Path("a.tsb"), &reader);
  ASSERT_TRUE(s.ok()) << s.ToString();
  ASSERT_EQ(reader->blocks().size(), 1U);
  const BlockMeta& meta = reader->blocks()[0];
  EXPECT_EQ(meta.series_id, 42U);
  EXPECT_EQ(meta.min_timestamp, 1000);
  EXPECT_EQ(meta.max_timestamp, 1090);
  EXPECT_EQ(meta.count, 10U);
  EXPECT_EQ(meta.offset, kBlockFileHeaderSize);
  EXPECT_EQ(meta.min, 0.0);
  EXPECT_EQ(meta.max, 4.5);
  EXPECT_EQ(meta.sum, 22.5);
  CheckRange(*reader, 42, points, TimeRange());
}

TEST_F(BlockFileTest, SplitsALongSeriesIntoBlocks) {
  const Points points = MakePoints(0, 7, 2500);
  ASSERT_NO_FATAL_FAILURE(WriteOneSeries(Path("a.tsb"), 1, points));
  std::unique_ptr<BlockFileReader> reader;
  ASSERT_TRUE(BlockFileReader::Open(Path("a.tsb"), &reader).ok());

  ASSERT_EQ(reader->blocks().size(), 3U);
  EXPECT_EQ(reader->blocks()[0].count, 1024U);
  EXPECT_EQ(reader->blocks()[1].count, 1024U);
  EXPECT_EQ(reader->blocks()[2].count, 452U);
  EXPECT_EQ(reader->blocks()[1].min_timestamp, 7 * 1024);
  EXPECT_EQ(reader->blocks()[2].max_timestamp, 7 * 2499);

  CheckRange(*reader, 1, points, TimeRange());
  // Ranges that sit inside one block, straddle a block boundary, and touch the edges.
  CheckRange(*reader, 1, points, TimeRange{7 * 100, 7 * 200});
  CheckRange(*reader, 1, points, TimeRange{7 * 1000, 7 * 1100});
  CheckRange(*reader, 1, points, TimeRange{7 * 1023, 7 * 1024});
  CheckRange(*reader, 1, points, TimeRange{7 * 1024, 7 * 1024});
  CheckRange(*reader, 1, points, TimeRange{-100, 3});
  CheckRange(*reader, 1, points, TimeRange{7 * 2499, 7 * 5000});
  // Ranges with nothing in them.
  CheckRange(*reader, 1, points, TimeRange{5, 6});
  CheckRange(*reader, 1, points, TimeRange{100000, 200000});
  CheckRange(*reader, 1, points, TimeRange{-50, -10});
  CheckRange(*reader, 1, points, TimeRange{700, 70});
}

TEST_F(BlockFileTest, BlockStatisticsMatchTheData) {
  const Points points = MakePoints(0, 1, 2500);
  ASSERT_NO_FATAL_FAILURE(WriteOneSeries(Path("a.tsb"), 1, points));
  std::unique_ptr<BlockFileReader> reader;
  ASSERT_TRUE(BlockFileReader::Open(Path("a.tsb"), &reader).ok());

  size_t next = 0;
  for (const BlockMeta& meta : reader->blocks()) {
    double sum = 0.0;
    double min = points.values[next];
    double max = points.values[next];
    for (size_t i = next; i < next + meta.count; ++i) {
      sum += points.values[i];
      min = std::min(min, points.values[i]);
      max = std::max(max, points.values[i]);
    }
    EXPECT_EQ(meta.min_timestamp, points.timestamps[next]);
    EXPECT_EQ(meta.max_timestamp, points.timestamps[next + meta.count - 1]);
    EXPECT_EQ(meta.sum, sum);
    EXPECT_EQ(meta.min, min);
    EXPECT_EQ(meta.max, max);
    next += meta.count;
  }
  EXPECT_EQ(next, points.size());
}

TEST_F(BlockFileTest, KeepsSeriesApart) {
  const Points a = MakePoints(0, 10, 100);
  const Points b = MakePoints(5, 10, 1500);
  const Points c = MakePoints(-500, 3, 20);
  std::unique_ptr<BlockFileWriter> writer;
  ASSERT_TRUE(BlockFileWriter::Create(Path("a.tsb"), &writer).ok());
  ASSERT_TRUE(writer->AddSeries(3, a).ok());
  ASSERT_TRUE(writer->AddSeries(7, b).ok());
  ASSERT_TRUE(writer->AddSeries(9, c).ok());
  ASSERT_TRUE(writer->Finish().ok());

  std::unique_ptr<BlockFileReader> reader;
  ASSERT_TRUE(BlockFileReader::Open(Path("a.tsb"), &reader).ok());
  EXPECT_EQ(reader->blocks().size(), 4U);
  CheckRange(*reader, 3, a, TimeRange());
  CheckRange(*reader, 7, b, TimeRange());
  CheckRange(*reader, 7, b, TimeRange{10000, 10400});
  CheckRange(*reader, 9, c, TimeRange());
  // Ids below, between and above the ones in the file.
  CheckRange(*reader, 1, Points(), TimeRange());
  CheckRange(*reader, 5, Points(), TimeRange());
  CheckRange(*reader, 100, Points(), TimeRange());
}

TEST_F(BlockFileTest, EmptyFileIsValid) {
  std::unique_ptr<BlockFileWriter> writer;
  ASSERT_TRUE(BlockFileWriter::Create(Path("a.tsb"), &writer).ok());
  ASSERT_TRUE(writer->AddSeries(1, Points()).ok());
  ASSERT_TRUE(writer->Finish().ok());

  std::unique_ptr<BlockFileReader> reader;
  const Status s = BlockFileReader::Open(Path("a.tsb"), &reader);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_TRUE(reader->blocks().empty());
  CheckRange(*reader, 1, Points(), TimeRange());
}

TEST_F(BlockFileTest, RejectsBadInput) {
  std::unique_ptr<BlockFileWriter> writer;
  ASSERT_TRUE(BlockFileWriter::Create(Path("a.tsb"), &writer).ok());

  Points unsorted;
  unsorted.timestamps = {1, 3, 2};
  unsorted.values = {0.0, 0.0, 0.0};
  EXPECT_EQ(writer->AddSeries(1, unsorted).code(), Status::Code::kInvalidArgument);

  Points repeated;
  repeated.timestamps = {1, 1};
  repeated.values = {0.0, 0.0};
  EXPECT_EQ(writer->AddSeries(1, repeated).code(), Status::Code::kInvalidArgument);

  Points mismatched;
  mismatched.timestamps = {1, 2};
  mismatched.values = {0.0};
  EXPECT_EQ(writer->AddSeries(1, mismatched).code(), Status::Code::kInvalidArgument);

  const Points good = MakePoints(0, 1, 3);
  ASSERT_TRUE(writer->AddSeries(5, good).ok());
  EXPECT_EQ(writer->AddSeries(5, good).code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(writer->AddSeries(4, good).code(), Status::Code::kInvalidArgument);

  ASSERT_TRUE(writer->Finish().ok());
  EXPECT_EQ(writer->Finish().code(), Status::Code::kInvalidArgument);
  EXPECT_EQ(writer->AddSeries(6, good).code(), Status::Code::kInvalidArgument);

  std::unique_ptr<BlockFileWriter> second;
  const Status s = BlockFileWriter::Create(Path("a.tsb"), &second);
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

// A crash during a flush must not leave something that looks like a finished file.
TEST_F(BlockFileTest, UnfinishedFileNeverAppearsUnderItsName) {
  {
    std::unique_ptr<BlockFileWriter> writer;
    ASSERT_TRUE(BlockFileWriter::Create(Path("a.tsb"), &writer).ok());
    ASSERT_TRUE(writer->AddSeries(1, MakePoints(0, 1, 5000)).ok());
    EXPECT_FALSE(std::filesystem::exists(Path("a.tsb")));
    EXPECT_TRUE(std::filesystem::exists(Path("a.tsb.tmp")));
  }
  EXPECT_FALSE(std::filesystem::exists(Path("a.tsb")));
  EXPECT_FALSE(std::filesystem::exists(Path("a.tsb.tmp")));
}

TEST_F(BlockFileTest, DamagedBlockFailsItsChecksum) {
  const std::string path = Path("a.tsb");
  ASSERT_NO_FATAL_FAILURE(WriteOneSeries(path, 1, MakePoints(0, 10, 100)));
  std::string bytes = ReadFileBytes(path);
  const size_t pos = kBlockFileHeaderSize + 20;
  bytes[pos] = static_cast<char>(bytes[pos] ^ 0x04);
  WriteFileBytes(path, bytes);

  // The index is intact, so the file opens; the damage shows when the block is read.
  std::unique_ptr<BlockFileReader> reader;
  ASSERT_TRUE(BlockFileReader::Open(path, &reader).ok());
  Points got;
  EXPECT_EQ(reader->ReadRange(1, TimeRange(), &got).code(), Status::Code::kCorruption);
}

TEST_F(BlockFileTest, DamagedIndexOrFooterFailsOpen) {
  const std::string path = Path("a.tsb");
  ASSERT_NO_FATAL_FAILURE(WriteOneSeries(path, 1, MakePoints(0, 10, 100)));
  const std::string bytes = ReadFileBytes(path);
  const size_t tail = kBlockIndexEntrySize + kBlockFileFooterSize;
  ASSERT_GT(bytes.size(), tail);

  for (size_t pos = bytes.size() - tail; pos < bytes.size(); ++pos) {
    std::string damaged = bytes;
    damaged[pos] = static_cast<char>(damaged[pos] ^ 0x01);
    WriteFileBytes(path, damaged);
    std::unique_ptr<BlockFileReader> reader;
    const Status s = BlockFileReader::Open(path, &reader);
    EXPECT_EQ(s.code(), Status::Code::kCorruption) << "pos=" << pos;
  }
}

TEST_F(BlockFileTest, TruncatedOrForeignFileFailsOpen) {
  const std::string path = Path("a.tsb");
  ASSERT_NO_FATAL_FAILURE(WriteOneSeries(path, 1, MakePoints(0, 10, 100)));
  const std::string bytes = ReadFileBytes(path);
  std::unique_ptr<BlockFileReader> reader;

  const std::vector<size_t> lengths = {0, 8, 31, bytes.size() / 2, bytes.size() - 1};
  for (const size_t len : lengths) {
    WriteFileBytes(path, bytes.substr(0, len));
    EXPECT_EQ(BlockFileReader::Open(path, &reader).code(), Status::Code::kCorruption) << len;
  }

  WriteFileBytes(path, std::string(200, 'x'));
  EXPECT_EQ(BlockFileReader::Open(path, &reader).code(), Status::Code::kCorruption);
}

}  // namespace
}  // namespace tsengine
