#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "tsengine/status.h"
#include "tsengine/types.h"

namespace tsengine {

// The in-memory home of recent points: one append buffer per series.
//
// Points may arrive in any order and a timestamp may be written more than once. Appending
// never sorts; a series is put in order only when it is read or flushed. If a timestamp was
// written several times, the last write wins.
//
// Not thread-safe.
class Memtable {
 public:
  void Append(uint64_t series_id, int64_t timestamp_ns, double value);

  // Number of Append calls, counting overwritten points too.
  size_t point_count() const { return point_count_; }
  bool empty() const { return point_count_ == 0; }

  // Ids of every series that has points, in increasing order.
  std::vector<uint64_t> SeriesIds() const;

  // Appends to `out` the points of `series_id` inside `range`, in time order, with repeated
  // timestamps reduced to their last write.
  void ReadRange(uint64_t series_id, TimeRange range, Points* out) const;

  // The earliest and latest timestamp held for a series. Returns false if it has no points.
  bool TimeBounds(uint64_t series_id, TimeRange* bounds) const;

 private:
  struct Series {
    Points points;
    // True while timestamps have arrived strictly increasing, so no sorting is needed.
    bool ordered = true;
    int64_t min_timestamp = 0;
    int64_t max_timestamp = 0;
  };

  std::unordered_map<uint64_t, Series> series_;
  size_t point_count_ = 0;
};

// Writes every series of `memtable` to a new block file at `path`.
Status FlushMemtable(const Memtable& memtable, const std::string& path);

}  // namespace tsengine
