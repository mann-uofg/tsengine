#pragma once

#include <cstdint>
#include <string>

namespace tsengine {

// Fixed-width little-endian integers for on-disk formats. Bytes are produced and consumed
// with shifts, so the result does not depend on the host byte order, on alignment, or on
// how the compiler lays out a struct.

inline void EncodeFixed16(char* dst, uint16_t v) {
  dst[0] = static_cast<char>(v & 0xff);
  dst[1] = static_cast<char>((v >> 8) & 0xff);
}

inline void EncodeFixed32(char* dst, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  }
}

inline void EncodeFixed64(char* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((v >> (8 * i)) & 0xff);
  }
}

inline uint16_t DecodeFixed16(const char* p) {
  const auto lo = static_cast<uint16_t>(static_cast<unsigned char>(p[0]));
  const auto hi = static_cast<uint16_t>(static_cast<unsigned char>(p[1]));
  return static_cast<uint16_t>(lo | (hi << 8));
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return v;
}

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  EncodeFixed64(buf, v);
  dst->append(buf, sizeof(buf));
}

}  // namespace tsengine
