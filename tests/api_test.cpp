#include "tsengine/api.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "test_util.h"
#include "tsengine/engine.h"

namespace tsengine {
namespace {

// {"name":"<name>","timestamps":[<ts>],"values":[<vs>]}
std::string Result(const std::string& name, const std::string& ts, const std::string& vs) {
  return "{\"name\":\"" + name + "\",\"timestamps\":[" + ts + "],\"values\":[" + vs + "]}";
}

// The whole response to a query that touched no block files.
std::string Envelope(const std::string& results, int series) {
  std::string json = "{\"results\":[" + results + "],\"stats\":{\"series\":";
  json += std::to_string(series);
  json += ",\"blocks_from_stats\":0,\"blocks_decoded\":0}}";
  return json;
}

bool Contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

class ApiTest : public TempDirTest {
 protected:
  void SetUp() override {
    TempDirTest::SetUp();
    options_.sync_writes = false;
    options_.compaction_min_files = 0;
    const Status s = Engine::Open(Path("db"), options_, &engine_);
    ASSERT_TRUE(s.ok()) << s.ToString();
    api_ = std::make_unique<Api>(engine_.get());
  }

  Status Write(const std::string& body) {
    WriteRequest request;
    request.body = body;
    request.timestamp_unit_ns = unit_ns_;
    request.now_ns = 777;
    return api_->Write(request, &summary_);
  }

  std::string Query() {
    std::string json;
    const Status s = api_->Query(query_, &json);
    EXPECT_TRUE(s.ok()) << s.ToString();
    return json;
  }

  void Where(const std::string& key, const std::vector<std::string>& values) {
    TagFilter filter;
    filter.key = key;
    filter.values = values;
    query_.selector.tags.push_back(filter);
  }

  // Two hosts, two fields, three timestamps each.
  void WriteCpuData() {
    const std::string body =
        "cpu,host=a user=1,system=10 0\n"
        "cpu,host=a user=2,system=20 5\n"
        "cpu,host=a user=3,system=30 10\n"
        "cpu,host=b user=7,system=70 0\n"
        "cpu,host=b user=8,system=80 5\n"
        "cpu,host=b user=9,system=90 10\n";
    const Status s = Write(body);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  EngineOptions options_;
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<Api> api_;
  WriteSummary summary_;
  QueryRequest query_;
  int64_t unit_ns_ = 1;
};

TEST_F(ApiTest, EachFieldOfALineBecomesASeries) {
  ASSERT_NO_FATAL_FAILURE(WriteCpuData());
  EXPECT_EQ(summary_.lines, 6U);
  EXPECT_EQ(summary_.points, 12U);
  EXPECT_EQ(engine_->series_count(), 4U);

  query_.selector.measurement = "cpu";
  query_.selector.field = "user";
  const std::string a = Result("cpu,host=a user", "0,5,10", "1,2,3");
  const std::string b = Result("cpu,host=b user", "0,5,10", "7,8,9");
  EXPECT_EQ(Query(), Envelope(a + "," + b, 2));
}

TEST_F(ApiTest, RangeAndTagsNarrowARawQuery) {
  ASSERT_NO_FATAL_FAILURE(WriteCpuData());
  query_.selector.measurement = "cpu";
  query_.selector.field = "system";
  query_.range = TimeRange{5, 10};
  Where("host", {"b"});
  EXPECT_EQ(Query(), Envelope(Result("cpu,host=b system", "5,10", "80,90"), 1));
}

TEST_F(ApiTest, NoMatchGivesAnEmptyResult) {
  ASSERT_NO_FATAL_FAILURE(WriteCpuData());
  query_.selector.measurement = "disk";
  EXPECT_EQ(Query(), Envelope("", 0));
}

TEST_F(ApiTest, AggregateCombinesAllMatchingSeries) {
  ASSERT_NO_FATAL_FAILURE(WriteCpuData());
  query_.selector.measurement = "cpu";
  query_.selector.field = "user";
  query_.bucket_width_ns = 10;

  // Buckets [0,10) and [10,20). Host a has 1,2 then 3; host b has 7,8 then 9.
  query_.aggregate = AggregateKind::kMax;
  EXPECT_EQ(Query(), Envelope(Result("", "0,10", "8,9"), 2));
  query_.aggregate = AggregateKind::kMin;
  EXPECT_EQ(Query(), Envelope(Result("", "0,10", "1,3"), 2));
  query_.aggregate = AggregateKind::kSum;
  EXPECT_EQ(Query(), Envelope(Result("", "0,10", "18,12"), 2));
  query_.aggregate = AggregateKind::kCount;
  EXPECT_EQ(Query(), Envelope(Result("", "0,10", "4,2"), 2));
  query_.aggregate = AggregateKind::kMean;
  EXPECT_EQ(Query(), Envelope(Result("", "0,10", "4.5,6"), 2));
}

TEST_F(ApiTest, AggregateGroupsByTag) {
  ASSERT_NO_FATAL_FAILURE(WriteCpuData());
  query_.selector.measurement = "cpu";
  query_.selector.field = "user";
  query_.aggregate = AggregateKind::kMean;
  query_.range = TimeRange{0, 100};
  query_.group_by = "host";
  // No bucket width: one bucket for the whole range, starting where the range starts.
  EXPECT_EQ(Query(), Envelope(Result("a", "0", "2") + "," + Result("b", "0", "8"), 2));
}

TEST_F(ApiTest, AggregateGroupsByField) {
  ASSERT_NO_FATAL_FAILURE(WriteCpuData());
  query_.selector.measurement = "cpu";
  query_.aggregate = AggregateKind::kMax;
  query_.range = TimeRange{0, 100};
  query_.group_by = kGroupByField;
  Where("host", {"a"});
  const std::string system = Result("system", "0", "30");
  const std::string user = Result("user", "0", "3");
  EXPECT_EQ(Query(), Envelope(system + "," + user, 2));
}

TEST_F(ApiTest, AggregateReportsPushDown) {
  std::string body;
  for (int t = 0; t < 3000; ++t) {
    body += "m value=" + std::to_string(t % 10) + " " + std::to_string(t) + "\n";
  }
  ASSERT_TRUE(Write(body).ok());
  ASSERT_TRUE(api_->Flush().ok());

  // 3000 points in order are three blocks, and a count over all time needs none of them
  // decoded.
  query_.selector.measurement = "m";
  query_.aggregate = AggregateKind::kCount;
  const std::string json = Query();
  EXPECT_TRUE(Contains(json, "\"values\":[3000]")) << json;
  EXPECT_TRUE(Contains(json, "\"blocks_from_stats\":3,\"blocks_decoded\":0")) << json;
}

TEST_F(ApiTest, BlankLinesCommentsAndCarriageReturnsAreSkipped) {
  const Status s = Write("\n# a comment\r\ncpu value=1 10\r\n\r\n   \ncpu value=2 20");
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(summary_.lines, 2U);
  EXPECT_EQ(summary_.points, 2U);
  query_.selector.measurement = "cpu";
  EXPECT_EQ(Query(), Envelope(Result("cpu value", "10,20", "1,2"), 1));
}

TEST_F(ApiTest, OneBadLineRejectsTheWholeBody) {
  const Status s = Write("cpu value=1 10\ncpu value=oops 20\ncpu value=3 30\n");
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
  EXPECT_TRUE(Contains(s.message(), "line 2")) << s.message();
  // The whole body is parsed before anything is stored, so not even the series named on the
  // good first line was created.
  query_.selector.measurement = "cpu";
  EXPECT_EQ(Query(), Envelope("", 0));
  EXPECT_EQ(engine_->series_count(), 0U);
}

TEST_F(ApiTest, TimestampUnitAndDefaultTimestamp) {
  unit_ns_ = 1000000000;
  ASSERT_TRUE(Write("cpu value=1 5\ncpu value=2\n").ok());
  query_.selector.measurement = "cpu";
  // 5 seconds becomes 5e9 ns. The line without a timestamp gets the request's "now".
  EXPECT_EQ(Query(), Envelope(Result("cpu value", "777,5000000000", "2,1"), 1));
}

TEST_F(ApiTest, NamesAreEscapedInJson) {
  ASSERT_TRUE(Write("we\"ird value=1.5 3\n").ok());
  EXPECT_EQ(Query(), Envelope(Result("we\\\"ird value", "3", "1.5"), 1));
  const std::string error = Api::ErrorJson(Status::InvalidArgument("a \"b\"\n"));
  EXPECT_EQ(error, "{\"error\":\"Invalid argument: a \\\"b\\\"\\u000a\"}");
}

TEST_F(ApiTest, StatsDescribeTheEngine) {
  ASSERT_NO_FATAL_FAILURE(WriteCpuData());
  const std::string before = "{\"series\":4,\"block_files\":0,\"memtable_points\":12,";
  EXPECT_EQ(api_->StatsJson(), before + "\"wal_segment\":1}");
  ASSERT_TRUE(api_->Flush().ok());
  const std::string after = "{\"series\":4,\"block_files\":1,\"memtable_points\":0,";
  EXPECT_EQ(api_->StatsJson(), after + "\"wal_segment\":2}");
}

TEST(AggregateKindTest, ParsesNames) {
  AggregateKind kind = AggregateKind::kSum;
  EXPECT_TRUE(ParseAggregateKind("", &kind));
  EXPECT_EQ(kind, AggregateKind::kNone);
  EXPECT_TRUE(ParseAggregateKind("max", &kind));
  EXPECT_EQ(kind, AggregateKind::kMax);
  EXPECT_TRUE(ParseAggregateKind("avg", &kind));
  EXPECT_EQ(kind, AggregateKind::kMean);
  EXPECT_FALSE(ParseAggregateKind("median", &kind));
}

}  // namespace
}  // namespace tsengine
