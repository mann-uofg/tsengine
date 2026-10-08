#include "tsengine/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace tsengine {
namespace {

TEST(Coding, Fixed16IsLittleEndian) {
  char buf[2];
  EncodeFixed16(buf, 0x0201);
  EXPECT_EQ(std::string(buf, 2), std::string("\x01\x02", 2));
  EXPECT_EQ(DecodeFixed16(buf), 0x0201);
}

TEST(Coding, Fixed32IsLittleEndian) {
  std::string s;
  PutFixed32(&s, 0x04030201U);
  EXPECT_EQ(s, std::string("\x01\x02\x03\x04", 4));
  EXPECT_EQ(DecodeFixed32(s.data()), 0x04030201U);
}

TEST(Coding, Fixed64IsLittleEndian) {
  std::string s;
  PutFixed64(&s, 0x0807060504030201ULL);
  EXPECT_EQ(s, std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
  EXPECT_EQ(DecodeFixed64(s.data()), 0x0807060504030201ULL);
}

TEST(Coding, ExtremeValuesRoundTrip) {
  std::string s;
  PutFixed32(&s, std::numeric_limits<uint32_t>::max());
  PutFixed64(&s, std::numeric_limits<uint64_t>::max());
  PutFixed64(&s, 0);
  EXPECT_EQ(DecodeFixed32(s.data()), std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(DecodeFixed64(s.data() + 4), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(DecodeFixed64(s.data() + 12), 0U);
}

}  // namespace
}  // namespace tsengine
