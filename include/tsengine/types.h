#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace tsengine {

// A closed time interval: both ends are included. The default covers all of time.
struct TimeRange {
  int64_t min = std::numeric_limits<int64_t>::min();
  int64_t max = std::numeric_limits<int64_t>::max();

  bool Contains(int64_t t) const { return t >= min && t <= max; }
};

// The points of one series, stored as two parallel columns.
struct Points {
  std::vector<int64_t> timestamps;
  std::vector<double> values;

  size_t size() const { return timestamps.size(); }
  bool empty() const { return timestamps.empty(); }

  void clear() {
    timestamps.clear();
    values.clear();
  }

  void Append(int64_t timestamp, double value) {
    timestamps.push_back(timestamp);
    values.push_back(value);
  }
};

// Appends to `to` the points of `from` that fall inside `range`. `from` must be sorted by
// timestamp, which lets this find both ends with a binary search.
inline void AppendRange(const Points& from, TimeRange range, Points* to) {
  const auto begin = from.timestamps.begin();
  const auto first = std::lower_bound(begin, from.timestamps.end(), range.min);
  const auto last = std::upper_bound(first, from.timestamps.end(), range.max);
  const auto lo = first - begin;
  const auto hi = last - begin;
  to->timestamps.insert(to->timestamps.end(), first, last);
  to->values.insert(to->values.end(), from.values.begin() + lo, from.values.begin() + hi);
}

}  // namespace tsengine
