#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tsengine {

// Writes values of any bit width into a byte buffer, most significant bit first. The encoders
// need this because their fields are 1, 2, 5, 6... bits wide, not whole bytes.
class BitWriter {
 public:
  // Appends the low `nbits` bits of `value`. `nbits` must be between 0 and 64.
  void WriteBits(uint64_t value, int nbits) {
    while (nbits > 0) {
      const int used = static_cast<int>(bit_count_ % 8);
      if (used == 0) {
        bytes_.push_back('\0');
      }
      // Fill what is left of the current byte with the highest bits not yet written.
      const int room = 8 - used;
      const int take = nbits < room ? nbits : room;
      const uint64_t chunk = (value >> (nbits - take)) & ((uint64_t{1} << take) - 1);
      const auto current = static_cast<unsigned char>(bytes_.back());
      bytes_.back() = static_cast<char>(current | (chunk << (room - take)));
      nbits -= take;
      bit_count_ += static_cast<uint64_t>(take);
    }
  }

  void WriteBit(bool bit) { WriteBits(bit ? uint64_t{1} : uint64_t{0}, 1); }

  uint64_t bit_count() const { return bit_count_; }

  // The bytes written so far. Unused bits of the last byte are zero.
  const std::string& bytes() const { return bytes_; }

 private:
  std::string bytes_;
  uint64_t bit_count_ = 0;
};

// Reads back what a BitWriter wrote. Every read is bounds-checked, so corrupt input makes a
// read fail instead of running off the end of the buffer.
class BitReader {
 public:
  explicit BitReader(std::string_view data) : data_(data) {}

  // Reads `nbits` bits (0 to 64) into *value. Returns false if the input runs out.
  bool ReadBits(int nbits, uint64_t* value) {
    if (static_cast<uint64_t>(nbits) > data_.size() * 8 - pos_) {
      return false;
    }
    uint64_t result = 0;
    while (nbits > 0) {
      const int used = static_cast<int>(pos_ % 8);
      const int room = 8 - used;
      const int take = nbits < room ? nbits : room;
      const auto byte = static_cast<uint64_t>(static_cast<unsigned char>(data_[pos_ / 8]));
      const uint64_t chunk = (byte >> (room - take)) & ((uint64_t{1} << take) - 1);
      result = (result << take) | chunk;
      nbits -= take;
      pos_ += static_cast<uint64_t>(take);
    }
    *value = result;
    return true;
  }

  bool ReadBit(bool* bit) {
    uint64_t value = 0;
    if (!ReadBits(1, &value)) {
      return false;
    }
    *bit = value != 0;
    return true;
  }

 private:
  std::string_view data_;
  // Position of the next unread bit.
  uint64_t pos_ = 0;
};

}  // namespace tsengine
