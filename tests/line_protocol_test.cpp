#include "tsengine/line_protocol.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace tsengine {
namespace {

std::string SeriesKey(const ParsedLine& line, std::string_view field_key) {
  std::string key;
  AppendSeriesKey(line, field_key, &key);
  return key;
}

void ExpectRejected(std::string_view text) {
  ParsedLine line;
  EXPECT_FALSE(ParseLine(text, &line).ok()) << "accepted: '" << text << "'";
}

TEST(LineProtocol, ParsesTagsFieldsAndTimestamp) {
  const std::string text = "cpu,host=a,region=us user=58i,system=2.5 1451606400000000000";
  ParsedLine line;
  const Status s = ParseLine(text, &line);
  ASSERT_TRUE(s.ok()) << s.ToString();

  EXPECT_EQ(line.measurement, "cpu");
  ASSERT_EQ(line.tags.size(), 2U);
  EXPECT_EQ(line.tags[0].first, "host");
  EXPECT_EQ(line.tags[0].second, "a");
  EXPECT_EQ(line.tags[1].first, "region");
  EXPECT_EQ(line.tags[1].second, "us");
  ASSERT_EQ(line.fields.size(), 2U);
  EXPECT_EQ(line.fields[0].key, "user");
  EXPECT_EQ(line.fields[0].value, 58.0);
  EXPECT_EQ(line.fields[1].key, "system");
  EXPECT_EQ(line.fields[1].value, 2.5);
  EXPECT_TRUE(line.has_timestamp);
  EXPECT_EQ(line.timestamp_ns, 1451606400000000000);
}

TEST(LineProtocol, TagsAndTimestampAreOptional) {
  ParsedLine line;
  ASSERT_TRUE(ParseLine("cpu value=1", &line).ok());
  EXPECT_EQ(line.measurement, "cpu");
  EXPECT_TRUE(line.tags.empty());
  ASSERT_EQ(line.fields.size(), 1U);
  EXPECT_FALSE(line.has_timestamp);
  EXPECT_EQ(SeriesKey(line, "value"), "cpu value");
}

TEST(LineProtocol, SeriesKeyDoesNotDependOnTagOrder) {
  ParsedLine a;
  ParsedLine b;
  ASSERT_TRUE(ParseLine("cpu,region=us,host=a value=1", &a).ok());
  ASSERT_TRUE(ParseLine("cpu,host=a,region=us value=1", &b).ok());
  EXPECT_EQ(SeriesKey(a, "value"), "cpu,host=a,region=us value");
  EXPECT_EQ(SeriesKey(a, "value"), SeriesKey(b, "value"));
}

TEST(LineProtocol, EscapedSeparatorsStayInsideNames) {
  ParsedLine line;
  const Status s = ParseLine("my\\ cpu,host\\ name=a\\,b in\\=out=1 7", &line);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(line.measurement, "my\\ cpu");
  ASSERT_EQ(line.tags.size(), 1U);
  EXPECT_EQ(line.tags[0].first, "host\\ name");
  EXPECT_EQ(line.tags[0].second, "a\\,b");
  ASSERT_EQ(line.fields.size(), 1U);
  EXPECT_EQ(line.fields[0].key, "in\\=out");
  EXPECT_EQ(line.timestamp_ns, 7);
}

TEST(LineProtocol, NumericFieldTypes) {
  ParsedLine line;
  ASSERT_TRUE(ParseLine("m a=-1.5e3,b=-5i,c=18446744073709551615u,d=0 -12", &line).ok());
  ASSERT_EQ(line.fields.size(), 4U);
  EXPECT_EQ(line.fields[0].value, -1500.0);
  EXPECT_EQ(line.fields[1].value, -5.0);
  EXPECT_DOUBLE_EQ(line.fields[2].value, 18446744073709551615.0);
  EXPECT_EQ(line.fields[3].value, 0.0);
  EXPECT_EQ(line.timestamp_ns, -12);
}

TEST(LineProtocol, TrailingNewlineIsIgnored) {
  ParsedLine line;
  ASSERT_TRUE(ParseLine("cpu value=1 5\r\n", &line).ok());
  EXPECT_EQ(line.timestamp_ns, 5);
}

TEST(LineProtocol, ParsedLineCanBeReused) {
  ParsedLine line;
  ASSERT_TRUE(ParseLine("cpu,host=a x=1,y=2 9", &line).ok());
  ASSERT_TRUE(ParseLine("mem z=3", &line).ok());
  EXPECT_EQ(line.measurement, "mem");
  EXPECT_TRUE(line.tags.empty());
  EXPECT_EQ(line.fields.size(), 1U);
  EXPECT_FALSE(line.has_timestamp);
}

// One line of the TSBS cpu-only workload, the dataset the benchmarks use.
TEST(LineProtocol, ParsesATsbsCpuLine) {
  const std::string text =
      "cpu,hostname=host_0,region=eu-central-1,datacenter=eu-central-1a,rack=6,os=Ubuntu15.10,"
      "arch=x86,team=SF,service=19,service_version=1,service_environment=test "
      "usage_user=58i,usage_system=2i,usage_idle=24i,usage_nice=61i,usage_iowait=22i,"
      "usage_irq=63i,usage_softirq=6i,usage_steal=44i,usage_guest=80i,usage_guest_nice=38i "
      "1451606400000000000";
  ParsedLine line;
  const Status s = ParseLine(text, &line);
  ASSERT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(line.measurement, "cpu");
  EXPECT_EQ(line.tags.size(), 10U);
  ASSERT_EQ(line.fields.size(), 10U);
  EXPECT_EQ(line.fields[0].key, "usage_user");
  EXPECT_EQ(line.fields[0].value, 58.0);
  EXPECT_EQ(line.fields[9].key, "usage_guest_nice");
  EXPECT_EQ(line.fields[9].value, 38.0);
  EXPECT_EQ(line.timestamp_ns, 1451606400000000000);
}

TEST(LineProtocol, RejectsMalformedLines) {
  ExpectRejected("");
  ExpectRejected(" ");
  ExpectRejected("cpu");
  ExpectRejected("cpu ");
  ExpectRejected("cpu value");
  ExpectRejected("cpu value=");
  ExpectRejected("cpu =1");
  ExpectRejected(",host=a value=1");
  ExpectRejected("cpu,host value=1");
  ExpectRejected("cpu,host= value=1");
  ExpectRejected("cpu,=a value=1");
  ExpectRejected("cpu,host=a");
  ExpectRejected("cpu,host=a,host=b value=1");
  ExpectRejected("cpu value=1,");
  ExpectRejected("cpu value=1,,other=2");
  ExpectRejected("cpu value=1 abc");
  ExpectRejected("cpu value=1 12 34");
  ExpectRejected("cpu value=1i5");
  ExpectRejected("cpu value=i");
  ExpectRejected("cpu value=1.5i");
  ExpectRejected("cpu value=-1u");
}

TEST(LineProtocol, RejectsUnsupportedFieldTypes) {
  ExpectRejected("cpu value=\"text\"");
  ExpectRejected("cpu value=true");
  ExpectRejected("cpu value=nan");
  ExpectRejected("cpu value=inf");
}

// Cheap fuzzing: random strings glued together from the pieces the parser cares about. The
// sanitizers catch any out-of-bounds read; the assertions check accepted lines are well formed.
TEST(LineProtocol, RandomInputNeverCrashes) {
  const std::vector<std::string> pieces = {"m", ",", "=", " ", "1", "2i", "\\", "\"", "v=1"};
  std::mt19937 rng(12345);
  std::uniform_int_distribution<size_t> pick(0, pieces.size() - 1);
  std::uniform_int_distribution<size_t> count(0, 10);
  ParsedLine line;
  for (int i = 0; i < 50000; ++i) {
    std::string text;
    const size_t n = count(rng);
    for (size_t j = 0; j < n; ++j) {
      text += pieces[pick(rng)];
    }
    if (!ParseLine(text, &line).ok()) {
      continue;
    }
    EXPECT_FALSE(line.measurement.empty());
    EXPECT_FALSE(line.fields.empty());
    for (const LineField& field : line.fields) {
      EXPECT_FALSE(field.key.empty());
      EXPECT_FALSE(SeriesKey(line, field.key).empty());
    }
  }
}

}  // namespace
}  // namespace tsengine
