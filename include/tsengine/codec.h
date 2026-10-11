#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tsengine/status.h"

namespace tsengine {

// Column encoders for one block of a series, after the Gorilla paper (Pelkonen et al.,
// VLDB 2015, section 4.1). The point count is not stored in the stream; the caller keeps it.

// Timestamps: delta-of-delta.
//
// The first timestamp is stored whole (64 bits). For every later one we store how much the
// gap to the previous point changed, D = (t[i] - t[i-1]) - (t[i-1] - t[i-2]), taking the gap
// before the first point as 0. A series sampled at a fixed interval has D = 0 almost always,
// which costs one bit.
//
//   D == 0             ->  0
//   fits in 16 bits    ->  10   + 16 bits
//   fits in 32 bits    ->  110  + 32 bits
//   fits in 48 bits    ->  1110 + 48 bits
//   otherwise          ->  1111 + 64 bits
//
// The buckets are wider than Gorilla's (7/9/12/32 bits) because these timestamps are in
// nanoseconds, not seconds. Arithmetic wraps modulo 2^64, so any int64 sequence round-trips,
// sorted or not.
void EncodeTimestamps(std::span<const int64_t> timestamps, std::string* out);
Status DecodeTimestamps(std::string_view data, size_t count, std::vector<int64_t>* out);

// Values: Gorilla XOR.
//
// The first value is stored whole (64 bits). Every later one is XORed with the previous value.
// Neighbouring values usually share their sign, exponent and top mantissa bits, so the XOR is
// mostly zeros with a short run of meaningful bits in the middle.
//
//   XOR == 0                                   ->  0
//   meaningful bits fit the previous window    ->  10 + those bits
//   otherwise                                  ->  11 + leading zero count (5 bits)
//                                                     + meaningful bit count - 1 (6 bits)
//                                                     + the meaningful bits
//
// The "window" is the position and width of the meaningful bits of the last value that had
// to describe them explicitly. Values are compared as raw bits, so NaN and -0.0 round-trip.
void EncodeValues(std::span<const double> values, std::string* out);
Status DecodeValues(std::string_view data, size_t count, std::vector<double>* out);

}  // namespace tsengine
