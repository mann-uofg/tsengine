#include "tsengine/codec.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace tsengine {
namespace {

std::vector<uint64_t> Bits(const std::vector<double>& values) {
  std::vector<uint64_t> bits;
  for (const double v : values) {
    bits.push_back(std::bit_cast<uint64_t>(v));
  }
  return bits;
}

// Encodes, decodes and checks the result is identical. Returns the encoded size in bytes.
size_t RoundTripTimestamps(const std::vector<int64_t>& timestamps) {
  std::string encoded;
  EncodeTimestamps(timestamps, &encoded);
  std::vector<int64_t> decoded;
  const Status s = DecodeTimestamps(encoded, timestamps.size(), &decoded);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(decoded, timestamps);
  return encoded.size();
}

// Same for values, compared bit for bit so NaN and -0.0 count.
size_t RoundTripValues(const std::vector<double>& values) {
  std::string encoded;
  EncodeValues(values, &encoded);
  std::vector<double> decoded;
  const Status s = DecodeValues(encoded, values.size(), &decoded);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(Bits(decoded), Bits(values));
  return encoded.size();
}

TEST(TimestampCodec, EmptyAndSinglePoint) {
  EXPECT_EQ(RoundTripTimestamps({}), 0U);
  EXPECT_EQ(RoundTripTimestamps({1451606400000000000}), 8U);
}

// The TSBS shape: one point every 10 seconds.
TEST(TimestampCodec, RegularIntervalCostsOneBitPerPoint) {
  std::vector<int64_t> ts;
  for (int64_t i = 0; i < 1000; ++i) {
    ts.push_back(1451606400000000000 + i * 10000000000);
  }
  // 64 bits for the first point, 4 + 48 for the first gap, then 998 single bits: 1114 bits.
  EXPECT_EQ(RoundTripTimestamps(ts), 140U);
}

// Pins which bucket a delta-of-delta lands in. The first two points cost 64 + 18 bits.
TEST(TimestampCodec, PicksTheSmallestBucket) {
  EXPECT_EQ(RoundTripTimestamps({0, 1000, 2000}), 11U);
  EXPECT_EQ(RoundTripTimestamps({0, 1000, 2000 + 32767}), 13U);
  EXPECT_EQ(RoundTripTimestamps({0, 1000, 2000 - 32768}), 13U);
  EXPECT_EQ(RoundTripTimestamps({0, 1000, 2000 + 32768}), 15U);
  EXPECT_EQ(RoundTripTimestamps({0, 1000, 2000 - 32769}), 15U);
}

TEST(TimestampCodec, BucketBoundariesRoundTrip) {
  for (const int bits : {16, 32, 48}) {
    const int64_t limit = int64_t{1} << (bits - 1);
    for (const int64_t dod : {limit - 1, limit, -limit, -limit - 1}) {
      RoundTripTimestamps({0, 1000, 2000 + dod, 3000 + 2 * dod});
    }
  }
}

TEST(TimestampCodec, ExtremeValuesWrapCorrectly) {
  const int64_t lo = std::numeric_limits<int64_t>::min();
  const int64_t hi = std::numeric_limits<int64_t>::max();
  RoundTripTimestamps({lo, hi, 0, -1, hi, lo, lo, 1});
  RoundTripTimestamps({hi, lo});
}

TEST(TimestampCodec, UnsortedAndDuplicateInputRoundTrips) {
  RoundTripTimestamps({5, 5, 3, 10, 10, -7});
  RoundTripTimestamps({0, 0, 0, 0});
}

TEST(TimestampCodec, RandomSeriesRoundTrip) {
  std::mt19937_64 rng(1);
  for (int round = 0; round < 300; ++round) {
    const size_t n = rng() % 300;
    // 0 jitter bits is a perfectly regular series; 63 is close to random noise.
    const int jitter_bits = static_cast<int>(rng() % 64);
    std::vector<int64_t> ts;
    uint64_t t = rng();
    for (size_t i = 0; i < n; ++i) {
      t += 1000 + (rng() & ((uint64_t{1} << jitter_bits) - 1));
      ts.push_back(static_cast<int64_t>(t));
    }
    RoundTripTimestamps(ts);
  }
}

TEST(TimestampCodec, TruncatedInputIsCorruption) {
  const std::vector<int64_t> ts = {0, 10, 25, 1000000, 1000001, 5};
  std::string encoded;
  EncodeTimestamps(ts, &encoded);
  std::vector<int64_t> decoded;
  for (size_t len = 0; len < encoded.size(); ++len) {
    const Status s = DecodeTimestamps(encoded.substr(0, len), ts.size(), &decoded);
    EXPECT_EQ(s.code(), Status::Code::kCorruption) << "len=" << len;
  }
}

TEST(ValueCodec, EmptyAndSinglePoint) {
  EXPECT_EQ(RoundTripValues({}), 0U);
  EXPECT_EQ(RoundTripValues({3.25}), 8U);
}

TEST(ValueCodec, RepeatedValueCostsOneBitPerPoint) {
  const std::vector<double> values(1000, 42.5);
  // 64 bits for the first value, then 999 single bits: 1063 bits.
  EXPECT_EQ(RoundTripValues(values), 133U);
}

// 1.0 ^ 2.0 has 1 leading zero and 11 meaningful bits.
TEST(ValueCodec, WindowIsDescribedOnceThenReused) {
  // 64 + 1 bit.
  EXPECT_EQ(RoundTripValues({1.0, 1.0}), 9U);
  // 64 + (2 + 5 + 6 + 11) bits.
  EXPECT_EQ(RoundTripValues({1.0, 2.0}), 11U);
  // The third value fits the same window: 2 + 11 more bits.
  EXPECT_EQ(RoundTripValues({1.0, 2.0, 1.0}), 13U);
}

TEST(ValueCodec, SpecialValuesRoundTrip) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const double tiny = std::numeric_limits<double>::denorm_min();
  const double big = std::numeric_limits<double>::max();
  RoundTripValues({0.0, -0.0, nan, inf, -inf, tiny, big, -big, 1.0, nan, nan, 0.0});
}

// Whole numbers between 0 and 100, like the TSBS cpu fields.
TEST(ValueCodec, SmallIntegersCompress) {
  std::mt19937 rng(3);
  std::vector<double> values;
  for (int i = 0; i < 1000; ++i) {
    values.push_back(static_cast<double>(rng() % 101));
  }
  EXPECT_LT(RoundTripValues(values), 4000U);
}

TEST(ValueCodec, RandomSeriesRoundTrip) {
  std::mt19937_64 rng(2);
  for (int round = 0; round < 300; ++round) {
    const size_t n = rng() % 300;
    const int mode = static_cast<int>(rng() % 3);
    std::vector<double> values;
    double walk = 100.0;
    for (size_t i = 0; i < n; ++i) {
      if (mode == 0) {
        values.push_back(std::bit_cast<double>(rng()));
      } else if (mode == 1) {
        walk += static_cast<double>(rng() % 200) / 100.0 - 1.0;
        values.push_back(walk);
      } else {
        values.push_back(static_cast<double>(rng() % 4));
      }
    }
    RoundTripValues(values);
  }
}

TEST(ValueCodec, TruncatedInputIsCorruption) {
  const std::vector<double> values = {1.0, 1.0, 2.5, -3.75, 1e300, 0.0};
  std::string encoded;
  EncodeValues(values, &encoded);
  std::vector<double> decoded;
  for (size_t len = 0; len < encoded.size(); ++len) {
    const Status s = DecodeValues(encoded.substr(0, len), values.size(), &decoded);
    EXPECT_EQ(s.code(), Status::Code::kCorruption) << "len=" << len;
  }
}

// Cheap fuzzing: the decoders must reject or decode arbitrary bytes without reading out of
// bounds. The sanitizers in the debug build are what catch a failure here.
TEST(Codec, RandomBytesNeverCrashTheDecoders) {
  std::mt19937_64 rng(99);
  std::vector<int64_t> ts;
  std::vector<double> values;
  for (int round = 0; round < 20000; ++round) {
    std::string data(rng() % 64, '\0');
    for (char& c : data) {
      c = static_cast<char>(rng());
    }
    const size_t count = rng() % 200;
    if (DecodeTimestamps(data, count, &ts).ok()) {
      EXPECT_EQ(ts.size(), count);
    }
    if (DecodeValues(data, count, &values).ok()) {
      EXPECT_EQ(values.size(), count);
    }
  }
}

}  // namespace
}  // namespace tsengine
