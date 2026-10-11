#include "tsengine/crc32c.h"

#include <array>
#include <cstring>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace tsengine {
namespace {

// The Castagnoli polynomial 0x1EDC6F41 with its bits reversed, because this implementation
// processes each byte least-significant bit first.
constexpr uint32_t kPolynomial = 0x82F63B78U;

// table[b] is the CRC state after feeding the single byte b into a zero state. Processing a
// byte then becomes one table lookup instead of eight shift-and-xor steps.
constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 1U) != 0) {
        crc = (crc >> 1) ^ kPolynomial;
      } else {
        crc >>= 1;
      }
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = MakeTable();

uint32_t ExtendWithTable(uint32_t state, const unsigned char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const uint32_t index = (state ^ static_cast<uint32_t>(p[i])) & 0xFFU;
    state = kTable[index] ^ (state >> 8);
  }
  return state;
}

#if defined(__x86_64__)

// x86 processors since 2008 (SSE4.2) have an instruction that computes exactly this CRC,
// eight bytes at a time. It is several times faster than the table.
__attribute__((target("sse4.2"))) uint32_t ExtendWithSse42(uint32_t state, const unsigned char* p,
                                                           size_t n) {
  uint64_t wide = state;
  while (n >= 8) {
    uint64_t chunk;
    std::memcpy(&chunk, p, sizeof(chunk));
    wide = _mm_crc32_u64(wide, chunk);
    p += 8;
    n -= 8;
  }
  auto narrow = static_cast<uint32_t>(wide);
  while (n > 0) {
    narrow = _mm_crc32_u8(narrow, *p);
    ++p;
    --n;
  }
  return narrow;
}

// Checked once, when the program starts.
const bool kHasSse42 = __builtin_cpu_supports("sse4.2") != 0;

#endif

}  // namespace

uint32_t Crc32cExtendPortable(uint32_t crc, const void* data, size_t n) {
  // The state is kept inverted, so a run of leading zero bytes still changes the checksum.
  return ~ExtendWithTable(~crc, static_cast<const unsigned char*>(data), n);
}

uint32_t Crc32cExtend(uint32_t crc, const void* data, size_t n) {
#if defined(__x86_64__)
  if (kHasSse42) {
    return ~ExtendWithSse42(~crc, static_cast<const unsigned char*>(data), n);
  }
#endif
  return Crc32cExtendPortable(crc, data, n);
}

}  // namespace tsengine
