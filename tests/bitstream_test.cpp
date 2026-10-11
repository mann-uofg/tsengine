#include "tsengine/bitstream.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace tsengine {
namespace {

TEST(Bitstream, PacksMostSignificantBitFirst) {
  BitWriter w;
  w.WriteBits(0b101, 3);
  w.WriteBits(0b11111, 5);
  w.WriteBits(0b1, 1);
  EXPECT_EQ(w.bit_count(), 9U);
  EXPECT_EQ(w.bytes(), std::string("\xbf\x80", 2));
}

TEST(Bitstream, IgnoresBitsAboveTheRequestedWidth) {
  BitWriter w;
  w.WriteBits(0xFFFFFFFFFFFFFFFFULL, 4);
  w.WriteBits(0, 4);
  EXPECT_EQ(w.bytes(), std::string("\xf0", 1));
}

TEST(Bitstream, FullWidthValuesRoundTrip) {
  BitWriter w;
  w.WriteBits(0b011, 3);
  w.WriteBits(0xFFFFFFFFFFFFFFFFULL, 64);
  w.WriteBits(0, 64);
  w.WriteBits(0x0123456789ABCDEFULL, 64);
  EXPECT_EQ(w.bit_count(), 195U);

  BitReader r(w.bytes());
  uint64_t v = 0;
  ASSERT_TRUE(r.ReadBits(3, &v));
  EXPECT_EQ(v, 0b011U);
  ASSERT_TRUE(r.ReadBits(64, &v));
  EXPECT_EQ(v, 0xFFFFFFFFFFFFFFFFULL);
  ASSERT_TRUE(r.ReadBits(64, &v));
  EXPECT_EQ(v, 0U);
  ASSERT_TRUE(r.ReadBits(64, &v));
  EXPECT_EQ(v, 0x0123456789ABCDEFULL);
}

TEST(Bitstream, ReadingPastTheEndFails) {
  BitWriter w;
  w.WriteBits(0b1011, 4);
  BitReader r(w.bytes());
  uint64_t v = 0;
  // The four padding bits of the last byte are readable and are zero.
  ASSERT_TRUE(r.ReadBits(8, &v));
  EXPECT_EQ(v, 0b10110000U);
  EXPECT_FALSE(r.ReadBits(1, &v));
  EXPECT_TRUE(r.ReadBits(0, &v));

  BitReader empty("");
  bool bit = false;
  EXPECT_FALSE(empty.ReadBit(&bit));
}

TEST(Bitstream, RandomWidthsRoundTrip) {
  std::mt19937_64 rng(7);
  std::vector<std::pair<uint64_t, int>> items;
  BitWriter w;
  for (int i = 0; i < 5000; ++i) {
    const int nbits = static_cast<int>(rng() % 65);
    uint64_t value = rng();
    if (nbits < 64) {
      value &= (uint64_t{1} << nbits) - 1;
    }
    w.WriteBits(value, nbits);
    items.emplace_back(value, nbits);
  }

  BitReader r(w.bytes());
  for (const auto& [value, nbits] : items) {
    uint64_t got = 0;
    ASSERT_TRUE(r.ReadBits(nbits, &got));
    ASSERT_EQ(got, value);
  }
}

}  // namespace
}  // namespace tsengine
