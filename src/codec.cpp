#include "tsengine/codec.h"

#include <bit>

#include "tsengine/bitstream.h"

namespace tsengine {
namespace {

// Payload widths of the delta-of-delta buckets, in the order of their prefixes.
constexpr int kDodWidths[] = {16, 32, 48, 64};

// True if `v` can be stored as a two's complement integer of `bits` bits.
bool FitsSigned(int64_t v, int bits) {
  const int64_t limit = int64_t{1} << (bits - 1);
  return v >= -limit && v < limit;
}

// Turns the low `bits` bits of `v` back into a full-width two's complement value.
uint64_t SignExtend(uint64_t v, int bits) {
  if (bits == 64) {
    return v;
  }
  const uint64_t sign = uint64_t{1} << (bits - 1);
  return (v ^ sign) - sign;
}

void WriteDod(BitWriter* w, uint64_t dod) {
  const auto value = static_cast<int64_t>(dod);
  if (value == 0) {
    w->WriteBits(0b0, 1);
  } else if (FitsSigned(value, 16)) {
    w->WriteBits(0b10, 2);
    w->WriteBits(dod, 16);
  } else if (FitsSigned(value, 32)) {
    w->WriteBits(0b110, 3);
    w->WriteBits(dod, 32);
  } else if (FitsSigned(value, 48)) {
    w->WriteBits(0b1110, 4);
    w->WriteBits(dod, 48);
  } else {
    w->WriteBits(0b1111, 4);
    w->WriteBits(dod, 64);
  }
}

bool ReadDod(BitReader* r, uint64_t* dod) {
  bool bit = false;
  if (!r->ReadBit(&bit)) {
    return false;
  }
  if (!bit) {
    *dod = 0;
    return true;
  }
  // Count the remaining 1 bits of the prefix, up to three, to find the bucket.
  int bucket = 0;
  while (bucket < 3) {
    if (!r->ReadBit(&bit)) {
      return false;
    }
    if (!bit) {
      break;
    }
    ++bucket;
  }
  uint64_t raw = 0;
  if (!r->ReadBits(kDodWidths[bucket], &raw)) {
    return false;
  }
  *dod = SignExtend(raw, kDodWidths[bucket]);
  return true;
}

// Every point after the first takes at least one bit. A count that the data cannot possibly
// hold is rejected up front, before anything is allocated for it.
bool CountIsPlausible(std::string_view data, size_t count) {
  return data.size() >= 8 && count - 1 <= (data.size() - 8) * 8;
}

}  // namespace

void EncodeTimestamps(std::span<const int64_t> timestamps, std::string* out) {
  if (timestamps.empty()) {
    return;
  }
  BitWriter w;
  auto prev = static_cast<uint64_t>(timestamps[0]);
  uint64_t prev_delta = 0;
  w.WriteBits(prev, 64);
  for (size_t i = 1; i < timestamps.size(); ++i) {
    const auto cur = static_cast<uint64_t>(timestamps[i]);
    const uint64_t delta = cur - prev;
    WriteDod(&w, delta - prev_delta);
    prev = cur;
    prev_delta = delta;
  }
  out->append(w.bytes());
}

Status DecodeTimestamps(std::string_view data, size_t count, std::vector<int64_t>* out) {
  out->clear();
  if (count == 0) {
    return Status::OK();
  }
  if (!CountIsPlausible(data, count)) {
    return Status::Corruption("timestamp data is too short for its point count");
  }
  out->reserve(count);
  BitReader r(data);
  uint64_t prev = 0;
  if (!r.ReadBits(64, &prev)) {
    return Status::Corruption("timestamp data ends early");
  }
  out->push_back(static_cast<int64_t>(prev));
  uint64_t prev_delta = 0;
  for (size_t i = 1; i < count; ++i) {
    uint64_t dod = 0;
    if (!ReadDod(&r, &dod)) {
      return Status::Corruption("timestamp data ends early");
    }
    prev_delta += dod;
    prev += prev_delta;
    out->push_back(static_cast<int64_t>(prev));
  }
  return Status::OK();
}

void EncodeValues(std::span<const double> values, std::string* out) {
  if (values.empty()) {
    return;
  }
  BitWriter w;
  uint64_t prev = std::bit_cast<uint64_t>(values[0]);
  w.WriteBits(prev, 64);
  // The current window: how many zero bits precede the meaningful bits, and how many
  // meaningful bits there are. window_bits == 0 means no window has been set yet.
  int window_leading = 0;
  int window_bits = 0;
  for (size_t i = 1; i < values.size(); ++i) {
    const uint64_t cur = std::bit_cast<uint64_t>(values[i]);
    const uint64_t x = cur ^ prev;
    prev = cur;
    if (x == 0) {
      w.WriteBits(0b0, 1);
      continue;
    }
    int leading = std::countl_zero(x);
    const int trailing = std::countr_zero(x);
    // The leading zero count is stored in 5 bits, so it cannot say more than 31.
    if (leading > 31) {
      leading = 31;
    }
    const int window_trailing = 64 - window_leading - window_bits;
    if (window_bits != 0 && leading >= window_leading && trailing >= window_trailing) {
      w.WriteBits(0b10, 2);
      w.WriteBits(x >> window_trailing, window_bits);
    } else {
      window_leading = leading;
      window_bits = 64 - leading - trailing;
      w.WriteBits(0b11, 2);
      w.WriteBits(static_cast<uint64_t>(window_leading), 5);
      w.WriteBits(static_cast<uint64_t>(window_bits - 1), 6);
      w.WriteBits(x >> trailing, window_bits);
    }
  }
  out->append(w.bytes());
}

Status DecodeValues(std::string_view data, size_t count, std::vector<double>* out) {
  out->clear();
  if (count == 0) {
    return Status::OK();
  }
  if (!CountIsPlausible(data, count)) {
    return Status::Corruption("value data is too short for its point count");
  }
  out->reserve(count);
  BitReader r(data);
  uint64_t prev = 0;
  if (!r.ReadBits(64, &prev)) {
    return Status::Corruption("value data ends early");
  }
  out->push_back(std::bit_cast<double>(prev));
  int window_leading = 0;
  int window_bits = 0;
  for (size_t i = 1; i < count; ++i) {
    bool changed = false;
    if (!r.ReadBit(&changed)) {
      return Status::Corruption("value data ends early");
    }
    if (changed) {
      bool new_window = false;
      if (!r.ReadBit(&new_window)) {
        return Status::Corruption("value data ends early");
      }
      if (new_window) {
        uint64_t leading = 0;
        uint64_t bits_minus_one = 0;
        if (!r.ReadBits(5, &leading) || !r.ReadBits(6, &bits_minus_one)) {
          return Status::Corruption("value data ends early");
        }
        window_leading = static_cast<int>(leading);
        window_bits = static_cast<int>(bits_minus_one) + 1;
        if (window_leading + window_bits > 64) {
          return Status::Corruption("value data describes an impossible bit window");
        }
      } else if (window_bits == 0) {
        return Status::Corruption("value data reuses a bit window before defining one");
      }
      uint64_t meaningful = 0;
      if (!r.ReadBits(window_bits, &meaningful)) {
        return Status::Corruption("value data ends early");
      }
      prev ^= meaningful << (64 - window_leading - window_bits);
    }
    out->push_back(std::bit_cast<double>(prev));
  }
  return Status::OK();
}

}  // namespace tsengine
