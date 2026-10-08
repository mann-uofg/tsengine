#include "tsengine/wal.h"

#include <gtest/gtest.h>
#include <stdlib.h>

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tsengine/coding.h"
#include "tsengine/crc32c.h"

namespace tsengine {
namespace {

// Samples are compared bit for bit, so NaN payloads and negative zero must survive too.
struct SampleBits {
  uint64_t series_id = 0;
  uint64_t timestamp = 0;
  uint64_t value = 0;

  bool operator==(const SampleBits&) const = default;
};

struct Record {
  WalRecordType type = WalRecordType::kSamples;
  uint64_t series_id = 0;
  std::string key;
  std::vector<SampleBits> samples;

  bool operator==(const Record&) const = default;
};

Record DefRecord(uint64_t series_id, std::string_view key) {
  Record r;
  r.type = WalRecordType::kSeriesDef;
  r.series_id = series_id;
  r.key = std::string(key);
  return r;
}

Record SamplesRecord(std::span<const Sample> samples) {
  Record r;
  r.type = WalRecordType::kSamples;
  for (const Sample& s : samples) {
    SampleBits bits;
    bits.series_id = s.series_id;
    bits.timestamp = static_cast<uint64_t>(s.timestamp_ns);
    bits.value = std::bit_cast<uint64_t>(s.value);
    r.samples.push_back(bits);
  }
  return r;
}

class Collector : public WalReplayHandler {
 public:
  void OnSeriesDef(uint64_t series_id, std::string_view key) override {
    records.push_back(DefRecord(series_id, key));
  }

  void OnSamples(std::span<const Sample> samples) override {
    records.push_back(SamplesRecord(samples));
  }

  std::vector<Record> records;
};

struct Replayed {
  Status status;
  WalReplayResult result;
  std::vector<Record> records;
};

Replayed Replay(const std::string& path) {
  Replayed r;
  Collector collector;
  r.status = ReplayWalSegment(path, &collector, &r.result);
  r.records = std::move(collector.records);
  return r;
}

std::string ReadBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<Record> Prefix(const std::vector<Record>& records, size_t n) {
  std::vector<Record> prefix;
  for (size_t i = 0; i < n; ++i) {
    prefix.push_back(records[i]);
  }
  return prefix;
}

// Builds a record by hand, independently of WalWriter, following docs/design.md.
std::string RawRecord(uint8_t type, const std::string& payload) {
  std::string record(kWalRecordHeaderSize, '\0');
  record += payload;
  EncodeFixed32(record.data() + 4, static_cast<uint32_t>(payload.size()));
  record[8] = static_cast<char>(type);
  EncodeFixed32(record.data(), Crc32c(record.data() + 4, record.size() - 4));
  return record;
}

struct Workload {
  std::vector<Record> records;
  // File size after each record, i.e. the offset where the next record starts.
  std::vector<uint64_t> ends;
};

// Writes a small mix of series definitions and sample batches to a new segment.
void WriteWorkload(const std::string& path, Workload* workload) {
  std::unique_ptr<WalWriter> writer;
  ASSERT_TRUE(WalWriter::Create(path, WalOptions(), &writer).ok());
  const std::vector<Sample> batch1 = {{1, 100, 1.5}, {2, 100, -2.25}, {1, 200, 1.75}};
  const std::vector<Sample> batch2 = {{2, 300, 0.0}};

  ASSERT_TRUE(writer->AppendSeriesDef(1, "cpu,host=a usage_user").ok());
  workload->records.push_back(DefRecord(1, "cpu,host=a usage_user"));
  workload->ends.push_back(writer->size());

  ASSERT_TRUE(writer->AppendSeriesDef(2, "cpu,host=b usage_user").ok());
  workload->records.push_back(DefRecord(2, "cpu,host=b usage_user"));
  workload->ends.push_back(writer->size());

  ASSERT_TRUE(writer->AppendSamples(batch1).ok());
  workload->records.push_back(SamplesRecord(batch1));
  workload->ends.push_back(writer->size());

  ASSERT_TRUE(writer->AppendSamples(batch2).ok());
  workload->records.push_back(SamplesRecord(batch2));
  workload->ends.push_back(writer->size());
}

class WalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    std::string dir = (base / "tsengine_wal_XXXXXX").string();
    ASSERT_NE(::mkdtemp(dir.data()), nullptr);
    dir_ = dir;
    path_ = dir_ + "/00000001.wal";
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string dir_;
  std::string path_;
};

TEST_F(WalTest, RoundTrip) {
  Workload w;
  ASSERT_NO_FATAL_FAILURE(WriteWorkload(path_, &w));

  const Replayed r = Replay(path_);
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.records, w.records);
  EXPECT_EQ(r.result.records, w.records.size());
  EXPECT_EQ(r.result.valid_size, w.ends.back());
  EXPECT_FALSE(r.result.torn_tail);
}

TEST_F(WalTest, NewSegmentIsEmpty) {
  std::unique_ptr<WalWriter> writer;
  ASSERT_TRUE(WalWriter::Create(path_, WalOptions(), &writer).ok());
  EXPECT_EQ(writer->size(), kWalSegmentHeaderSize);

  const Replayed r = Replay(path_);
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_TRUE(r.records.empty());
  EXPECT_EQ(r.result.valid_size, kWalSegmentHeaderSize);
  EXPECT_FALSE(r.result.torn_tail);
}

TEST_F(WalTest, CreateFailsIfSegmentExists) {
  std::unique_ptr<WalWriter> first;
  ASSERT_TRUE(WalWriter::Create(path_, WalOptions(), &first).ok());
  std::unique_ptr<WalWriter> second;
  const Status s = WalWriter::Create(path_, WalOptions(), &second);
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

// Pins the on-disk format: the writer's output must equal bytes built by hand from the spec.
TEST_F(WalTest, WriterMatchesTheDocumentedLayout) {
  std::unique_ptr<WalWriter> writer;
  ASSERT_TRUE(WalWriter::Create(path_, WalOptions(), &writer).ok());
  ASSERT_TRUE(writer->AppendSeriesDef(7, "abc").ok());

  std::string expected(kWalSegmentHeaderSize, '\0');
  EncodeFixed32(expected.data(), kWalMagic);
  EncodeFixed16(expected.data() + 4, kWalVersion);
  EXPECT_EQ(expected.substr(0, 4), "TSWL");
  std::string payload;
  PutFixed64(&payload, 7);
  PutFixed32(&payload, 3);
  payload += "abc";
  expected += RawRecord(1, payload);
  EXPECT_EQ(ReadBytes(path_), expected);
}

// A crash can cut the file at any byte. Whatever the length, replay must return exactly the
// records that are completely present and report where the last good one ends.
TEST_F(WalTest, TruncationAtAnyOffsetYieldsARecordPrefix) {
  Workload w;
  ASSERT_NO_FATAL_FAILURE(WriteWorkload(path_, &w));
  const std::string bytes = ReadBytes(path_);
  ASSERT_EQ(bytes.size(), w.ends.back());
  const std::string cut_path = dir_ + "/cut.wal";

  for (size_t len = kWalSegmentHeaderSize; len <= bytes.size(); ++len) {
    WriteBytes(cut_path, bytes.substr(0, len));
    size_t complete = 0;
    uint64_t valid_size = kWalSegmentHeaderSize;
    while (complete < w.ends.size() && w.ends[complete] <= len) {
      valid_size = w.ends[complete];
      ++complete;
    }

    const Replayed r = Replay(cut_path);
    ASSERT_TRUE(r.status.ok()) << "len=" << len << ": " << r.status.ToString();
    EXPECT_EQ(r.records, Prefix(w.records, complete)) << "len=" << len;
    EXPECT_EQ(r.result.valid_size, valid_size) << "len=" << len;
    EXPECT_EQ(r.result.torn_tail, len != valid_size) << "len=" << len;
  }
}

TEST_F(WalTest, MissingSegmentHeaderIsCorruption) {
  Workload w;
  ASSERT_NO_FATAL_FAILURE(WriteWorkload(path_, &w));
  const std::string bytes = ReadBytes(path_);
  for (size_t len = 0; len < kWalSegmentHeaderSize; ++len) {
    WriteBytes(path_, bytes.substr(0, len));
    EXPECT_EQ(Replay(path_).status.code(), Status::Code::kCorruption) << "len=" << len;
  }
}

TEST_F(WalTest, BadMagicIsCorruption) {
  WriteBytes(path_, std::string(64, 'x'));
  EXPECT_EQ(Replay(path_).status.code(), Status::Code::kCorruption);
}

// Flip one bit anywhere in the record area. Replay must stop at the start of the record that
// contains it and never hand damaged data to the handler.
TEST_F(WalTest, AnyFlippedBitIsDetected) {
  Workload w;
  ASSERT_NO_FATAL_FAILURE(WriteWorkload(path_, &w));
  const std::string bytes = ReadBytes(path_);
  const std::string bad_path = dir_ + "/flipped.wal";

  size_t record = 0;
  for (size_t pos = kWalSegmentHeaderSize; pos < bytes.size(); ++pos) {
    if (pos >= w.ends[record]) {
      ++record;
    }
    const uint64_t record_start = record == 0 ? kWalSegmentHeaderSize : w.ends[record - 1];
    std::string flipped = bytes;
    flipped[pos] = static_cast<char>(flipped[pos] ^ 0x10);
    WriteBytes(bad_path, flipped);

    const Replayed r = Replay(bad_path);
    ASSERT_TRUE(r.status.ok()) << "pos=" << pos << ": " << r.status.ToString();
    EXPECT_EQ(r.records, Prefix(w.records, record)) << "pos=" << pos;
    EXPECT_EQ(r.result.valid_size, record_start) << "pos=" << pos;
    EXPECT_TRUE(r.result.torn_tail) << "pos=" << pos;
  }
}

// Some filesystems leave zero bytes at the end of a file after a crash.
TEST_F(WalTest, ZeroFilledTailIsNotARecord) {
  Workload w;
  ASSERT_NO_FATAL_FAILURE(WriteWorkload(path_, &w));
  WriteBytes(path_, ReadBytes(path_) + std::string(4096, '\0'));

  const Replayed r = Replay(path_);
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.records, w.records);
  EXPECT_EQ(r.result.valid_size, w.ends.back());
  EXPECT_TRUE(r.result.torn_tail);
}

// The case the truncate-on-reopen rule exists for: records appended after a torn record must
// still be found by the next replay.
TEST_F(WalTest, AppendAfterTornTailIsReplayable) {
  Workload w;
  ASSERT_NO_FATAL_FAILURE(WriteWorkload(path_, &w));
  std::string bytes = ReadBytes(path_);
  bytes.append("\x2a\x01\x11\x7f\x05", 5);
  WriteBytes(path_, bytes);

  const Replayed torn = Replay(path_);
  ASSERT_TRUE(torn.status.ok()) << torn.status.ToString();
  ASSERT_TRUE(torn.result.torn_tail);
  ASSERT_EQ(torn.result.valid_size, w.ends.back());

  std::unique_ptr<WalWriter> writer;
  const Status s = WalWriter::OpenExisting(path_, torn.result.valid_size, WalOptions(), &writer);
  ASSERT_TRUE(s.ok()) << s.ToString();
  const std::vector<Sample> batch = {{7, 900, 42.0}};
  ASSERT_TRUE(writer->AppendSamples(batch).ok());

  const Replayed r = Replay(path_);
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  std::vector<Record> expected = w.records;
  expected.push_back(SamplesRecord(batch));
  EXPECT_EQ(r.records, expected);
  EXPECT_FALSE(r.result.torn_tail);
}

TEST_F(WalTest, OpenExistingRejectsASizePastTheEnd) {
  Workload w;
  ASSERT_NO_FATAL_FAILURE(WriteWorkload(path_, &w));
  std::unique_ptr<WalWriter> writer;
  const Status s = WalWriter::OpenExisting(path_, w.ends.back() + 1, WalOptions(), &writer);
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST_F(WalTest, OversizedBatchIsRejectedWithoutDamage) {
  std::unique_ptr<WalWriter> writer;
  ASSERT_TRUE(WalWriter::Create(path_, WalOptions(), &writer).ok());
  const std::vector<Sample> too_big(kWalMaxPayloadSize / kWalSampleSize + 1);
  EXPECT_EQ(writer->AppendSamples(too_big).code(), Status::Code::kInvalidArgument);

  const std::vector<Sample> batch = {{1, 1, 1.0}};
  ASSERT_TRUE(writer->AppendSamples(batch).ok());
  const Replayed r = Replay(path_);
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  ASSERT_EQ(r.records.size(), 1U);
  EXPECT_EQ(r.records[0], SamplesRecord(batch));
}

// These records pass their checksum, so they are what was written. Replay must refuse them
// outright instead of treating them as a harmless torn tail.
TEST_F(WalTest, ValidChecksumWithBadContentsIsCorruption) {
  std::unique_ptr<WalWriter> writer;
  ASSERT_TRUE(WalWriter::Create(path_, WalOptions(), &writer).ok());
  const std::string header = ReadBytes(path_);

  WriteBytes(path_, header + RawRecord(2, "12345"));
  EXPECT_EQ(Replay(path_).status.code(), Status::Code::kCorruption);

  WriteBytes(path_, header + RawRecord(9, ""));
  EXPECT_EQ(Replay(path_).status.code(), Status::Code::kCorruption);

  std::string bad_key_len;
  PutFixed64(&bad_key_len, 1);
  PutFixed32(&bad_key_len, 100);
  bad_key_len += "abc";
  WriteBytes(path_, header + RawRecord(1, bad_key_len));
  EXPECT_EQ(Replay(path_).status.code(), Status::Code::kCorruption);
}

TEST_F(WalTest, RandomBatchesRoundTrip) {
  std::mt19937_64 rng(20261008);
  WalOptions options;
  options.sync_on_append = false;
  std::unique_ptr<WalWriter> writer;
  ASSERT_TRUE(WalWriter::Create(path_, options, &writer).ok());

  std::vector<Record> expected;
  for (int i = 0; i < 200; ++i) {
    if (rng() % 4 == 0) {
      const uint64_t id = rng();
      const std::string key(rng() % 64, 'k');
      ASSERT_TRUE(writer->AppendSeriesDef(id, key).ok());
      expected.push_back(DefRecord(id, key));
    } else {
      std::vector<Sample> batch(rng() % 50 + 1);
      for (Sample& s : batch) {
        s.series_id = rng();
        s.timestamp_ns = static_cast<int64_t>(rng());
        s.value = std::bit_cast<double>(rng());
      }
      ASSERT_TRUE(writer->AppendSamples(batch).ok());
      expected.push_back(SamplesRecord(batch));
    }
  }
  ASSERT_TRUE(writer->Sync().ok());

  const Replayed r = Replay(path_);
  ASSERT_TRUE(r.status.ok()) << r.status.ToString();
  EXPECT_EQ(r.records, expected);
  EXPECT_FALSE(r.result.torn_tail);
}

}  // namespace
}  // namespace tsengine
