#include "tsengine/series_index.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tsengine {
namespace {

using Ids = std::vector<uint64_t>;

class SeriesIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    index_.Add(1, "cpu,host=a,region=us usage_user");
    index_.Add(2, "cpu,host=a,region=us usage_system");
    index_.Add(3, "cpu,host=b,region=us usage_user");
    index_.Add(4, "cpu,host=c,region=eu usage_user");
    index_.Add(5, "mem,host=a used");
    index_.Add(6, "not a canonical key");
  }

  Ids Select(const std::string& measurement, const std::string& field) {
    selector_.measurement = measurement;
    selector_.field = field;
    return index_.Select(selector_);
  }

  void Where(const std::string& key, const std::vector<std::string>& values) {
    TagFilter filter;
    filter.key = key;
    filter.values = values;
    selector_.tags.push_back(filter);
  }

  SeriesIndex index_;
  SeriesSelector selector_;
};

TEST_F(SeriesIndexTest, EmptySelectorMatchesEverything) {
  EXPECT_EQ(index_.size(), 6U);
  EXPECT_EQ(Select("", ""), (Ids{1, 2, 3, 4, 5, 6}));
}

TEST_F(SeriesIndexTest, SelectsByMeasurementAndField) {
  EXPECT_EQ(Select("cpu", ""), (Ids{1, 2, 3, 4}));
  EXPECT_EQ(Select("cpu", "usage_user"), (Ids{1, 3, 4}));
  EXPECT_EQ(Select("", "usage_user"), (Ids{1, 3, 4}));
  EXPECT_EQ(Select("mem", "used"), (Ids{5}));
  EXPECT_EQ(Select("disk", ""), Ids());
  EXPECT_EQ(Select("mem", "usage_user"), Ids());
}

TEST_F(SeriesIndexTest, TagFilterMatchesAnyOfItsValues) {
  Where("host", {"a", "b"});
  EXPECT_EQ(Select("cpu", "usage_user"), (Ids{1, 3}));
  EXPECT_EQ(Select("", ""), (Ids{1, 2, 3, 5}));
}

TEST_F(SeriesIndexTest, AllTagFiltersMustMatch) {
  Where("region", {"us"});
  Where("host", {"a"});
  EXPECT_EQ(Select("cpu", ""), (Ids{1, 2}));
  Where("host", {"b"});
  EXPECT_EQ(Select("cpu", ""), Ids());
}

TEST_F(SeriesIndexTest, UnknownTagOrValueMatchesNothing) {
  Where("host", {"zzz"});
  EXPECT_EQ(Select("cpu", ""), Ids());
  selector_.tags.clear();
  Where("rack", {"1"});
  EXPECT_EQ(Select("cpu", ""), Ids());
  selector_.tags.clear();
  Where("host", {});
  EXPECT_EQ(Select("cpu", ""), Ids());
}

TEST_F(SeriesIndexTest, LooksUpKeysAndTagValues) {
  EXPECT_EQ(index_.KeyOf(4), "cpu,host=c,region=eu usage_user");
  EXPECT_EQ(index_.KeyOf(99), "");
  EXPECT_EQ(index_.TagValue(4, "host"), "c");
  EXPECT_EQ(index_.TagValue(4, "region"), "eu");
  EXPECT_EQ(index_.TagValue(4, "rack"), "");
  EXPECT_EQ(index_.TagValue(6, "host"), "");
  EXPECT_EQ(index_.TagValue(99, "host"), "");
}

}  // namespace
}  // namespace tsengine
