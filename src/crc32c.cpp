#include "tsengine/crc32c.h"

#include <array>

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

}  // namespace

uint32_t Crc32cExtend(uint32_t crc, const void* data, size_t n) {
  const auto* p = static_cast<const unsigned char*>(data);
  // The state is kept inverted, so a run of leading zero bytes still changes the checksum.
  uint32_t state = ~crc;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t index = (state ^ static_cast<uint32_t>(p[i])) & 0xFFU;
    state = kTable[index] ^ (state >> 8);
  }
  return ~state;
}

}  // namespace tsengine
