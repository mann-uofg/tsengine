#include "tsengine/crc32c.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

namespace tsengine {
namespace {

// Test vectors from RFC 3720 (iSCSI), section B.4.
TEST(Crc32c, StandardVectors) {
  std::string buf(32, '\0');
  EXPECT_EQ(Crc32c(buf.data(), buf.size()), 0x8A9136AAU);

  buf.assign(32, '\xff');
  EXPECT_EQ(Crc32c(buf.data(), buf.size()), 0x62A8AB43U);

  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<char>(i);
  }
  EXPECT_EQ(Crc32c(buf.data(), buf.size()), 0x46DD794EU);

  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = static_cast<char>(31 - i);
  }
  EXPECT_EQ(Crc32c(buf.data(), buf.size()), 0x113FDB5CU);
}

TEST(Crc32c, CheckValue) { EXPECT_EQ(Crc32c("123456789", 9), 0xE3069283U); }

TEST(Crc32c, EmptyInputIsZero) { EXPECT_EQ(Crc32c("", 0), 0U); }

TEST(Crc32c, ExtendMatchesOneShot) {
  const std::string data = "the quick brown fox jumps over the lazy dog";
  const uint32_t whole = Crc32c(data.data(), data.size());
  for (size_t split = 0; split <= data.size(); ++split) {
    const uint32_t first = Crc32c(data.data(), split);
    EXPECT_EQ(Crc32cExtend(first, data.data() + split, data.size() - split), whole);
  }
}

// Every length from 0 to 300 at several alignments, so the eight-byte loop, its tail and
// unaligned starts are all covered.
TEST(Crc32c, HardwareAndTableImplementationsAgree) {
  std::mt19937 rng(7);
  std::string data(320, '\0');
  for (char& c : data) {
    c = static_cast<char>(rng());
  }
  for (size_t offset = 0; offset < 8; ++offset) {
    for (size_t n = 0; n <= 300; ++n) {
      const uint32_t seed = static_cast<uint32_t>(rng());
      ASSERT_EQ(Crc32cExtend(seed, data.data() + offset, n),
                Crc32cExtendPortable(seed, data.data() + offset, n))
          << "offset=" << offset << " n=" << n;
    }
  }
}

TEST(Crc32c, DetectsASingleFlippedBit) {
  std::string data(64, 'a');
  const uint32_t original = Crc32c(data.data(), data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>(data[i] ^ 0x01);
    EXPECT_NE(Crc32c(data.data(), data.size()), original);
    data[i] = static_cast<char>(data[i] ^ 0x01);
  }
}

}  // namespace
}  // namespace tsengine
