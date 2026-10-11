#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "tsengine/block_file.h"
#include "tsengine/memtable.h"
#include "tsengine/status.h"
#include "tsengine/types.h"

namespace tsengine {

// Reads the points of one series inside `range` from a set of block files and, optionally,
// a memtable. `out` is replaced with the result, sorted by timestamp.
//
// `files` must be ordered oldest first, and the memtable counts as newer than every file.
// If a timestamp appears in more than one source, the newest source wins.
Status QueryRange(std::span<const BlockFileReader* const> files, const Memtable* memtable,
                  uint64_t series_id, TimeRange range, Points* out);

// Summary of the points that fall into one time bucket. Buckets can be combined, which is
// how the results for several series are merged into one.
struct Bucket {
  // First timestamp of the bucket.
  int64_t start = 0;
  uint64_t count = 0;
  double sum = 0.0;
  double min = 0.0;
  double max = 0.0;

  void Add(double value);
  void Merge(const Bucket& other);
};

struct AggregateRequest {
  uint64_t series_id = 0;
  TimeRange range;
  // Buckets are [k * width, (k + 1) * width). A width of 0 or less means one bucket for the
  // whole range, which starts at range.min.
  int64_t bucket_width = 0;
};

// How the work of an aggregate query was done.
struct AggregateStats {
  // Blocks answered from the statistics in their index entry, without being read.
  uint64_t blocks_from_stats = 0;
  // Blocks that had to be read and decoded.
  uint64_t blocks_decoded = 0;
};

struct AggregateResult {
  // The buckets that contain at least one point, in time order.
  std::vector<Bucket> buckets;
  AggregateStats stats;
};

// Computes count, sum, min and max per time bucket for one series. out->buckets is replaced;
// out->stats is added to, so one result can collect the statistics of several calls.
//
// Aggregation push-down: a block is answered from its index entry alone when
//   - it lies entirely inside the query range,
//   - all of it falls into a single bucket, and
//   - no other file and not the memtable holds points of the series in its time span (those
//     could overwrite points in it, which the statistics cannot know about).
// Every other block is decoded and its points are bucketed one by one.
Status AggregateSeries(std::span<const BlockFileReader* const> files, const Memtable* memtable,
                       const AggregateRequest& request, AggregateResult* out);

}  // namespace tsengine
