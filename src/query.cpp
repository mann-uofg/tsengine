#include "tsengine/query.h"

#include <algorithm>
#include <utility>

namespace tsengine {
namespace {

// Merges `newer` into `acc`. Both are sorted by timestamp. Where both have the same
// timestamp, the point from `newer` replaces the one in `acc`.
void MergeNewer(const Points& newer, Points* acc) {
  if (newer.empty()) {
    return;
  }
  // The common case: sources cover different times, so this is a plain append.
  if (acc->empty() || acc->timestamps.back() < newer.timestamps.front()) {
    AppendRange(newer, TimeRange(), acc);
    return;
  }

  Points merged;
  merged.timestamps.reserve(acc->size() + newer.size());
  merged.values.reserve(acc->size() + newer.size());
  size_t i = 0;
  size_t j = 0;
  while (i < acc->size() && j < newer.size()) {
    if (acc->timestamps[i] < newer.timestamps[j]) {
      merged.Append(acc->timestamps[i], acc->values[i]);
      ++i;
    } else {
      if (acc->timestamps[i] == newer.timestamps[j]) {
        ++i;
      }
      merged.Append(newer.timestamps[j], newer.values[j]);
      ++j;
    }
  }
  for (; i < acc->size(); ++i) {
    merged.Append(acc->timestamps[i], acc->values[i]);
  }
  for (; j < newer.size(); ++j) {
    merged.Append(newer.timestamps[j], newer.values[j]);
  }
  *acc = std::move(merged);
}

}  // namespace

Status QueryRange(std::span<const BlockFileReader* const> files, const Memtable* memtable,
                  uint64_t series_id, TimeRange range, Points* out) {
  out->clear();
  Points run;
  for (const BlockFileReader* file : files) {
    run.clear();
    Status s = file->ReadRange(series_id, range, &run);
    if (!s.ok()) {
      return s;
    }
    MergeNewer(run, out);
  }
  if (memtable != nullptr) {
    run.clear();
    memtable->ReadRange(series_id, range, &run);
    MergeNewer(run, out);
  }
  return Status::OK();
}

void Bucket::Add(double value) {
  if (count == 0) {
    min = value;
    max = value;
  } else {
    min = std::min(min, value);
    max = std::max(max, value);
  }
  sum += value;
  ++count;
}

void Bucket::Merge(const Bucket& other) {
  if (other.count == 0) {
    return;
  }
  if (count == 0) {
    min = other.min;
    max = other.max;
  } else {
    min = std::min(min, other.min);
    max = std::max(max, other.max);
  }
  sum += other.sum;
  count += other.count;
}

namespace {

// The stretch of time covered by one source of points for the series: either one block of
// a file, or everything the memtable holds for the series (block == nullptr).
struct Span {
  int64_t min = 0;
  int64_t max = 0;
  const BlockMeta* block = nullptr;
};

bool StartsBefore(const Span& a, const Span& b) {
  if (a.min != b.min) {
    return a.min < b.min;
  }
  return a.max < b.max;
}

// First timestamp of the bucket that `timestamp` falls into.
int64_t BucketStart(int64_t timestamp, const AggregateRequest& request) {
  if (request.bucket_width <= 0) {
    return request.range.min;
  }
  return FloorDiv(timestamp, request.bucket_width) * request.bucket_width;
}

// Returns the bucket that starts at `start`. Buckets are filled in time order, so the one
// wanted is either the last one or a new one to append.
Bucket* BucketFor(int64_t start, std::vector<Bucket>* buckets) {
  if (buckets->empty() || buckets->back().start != start) {
    Bucket bucket;
    bucket.start = start;
    buckets->push_back(bucket);
  }
  return &buckets->back();
}

}  // namespace

Status AggregateSeries(std::span<const BlockFileReader* const> files, const Memtable* memtable,
                       const AggregateRequest& request, AggregateResult* out) {
  out->buckets.clear();
  const TimeRange range = request.range;
  if (range.min > range.max) {
    return Status::OK();
  }

  // Collect the time span of every source that could hold points of this series in the
  // range. For files this only looks at index entries; nothing is read yet.
  std::vector<Span> spans;
  std::vector<const BlockMeta*> blocks;
  for (const BlockFileReader* file : files) {
    blocks.clear();
    file->FindBlocks(request.series_id, range, &blocks);
    for (const BlockMeta* block : blocks) {
      spans.push_back(Span{block->min_timestamp, block->max_timestamp, block});
    }
  }
  TimeRange in_memtable;
  if (memtable != nullptr && memtable->TimeBounds(request.series_id, &in_memtable)) {
    if (in_memtable.max >= range.min && in_memtable.min <= range.max) {
      spans.push_back(Span{in_memtable.min, in_memtable.max, nullptr});
    }
  }
  std::sort(spans.begin(), spans.end(), StartsBefore);

  Points points;
  size_t i = 0;
  while (i < spans.size()) {
    // Group this span with every span that overlaps it, directly or through another one.
    // Different groups never share a timestamp, so each can be handled on its own.
    size_t j = i + 1;
    int64_t group_max = spans[i].max;
    while (j < spans.size() && spans[j].min <= group_max) {
      group_max = std::max(group_max, spans[j].max);
      ++j;
    }
    const Span& first = spans[i];
    const bool alone = j == i + 1 && first.block != nullptr;
    const bool inside_range = first.min >= range.min && first.max <= range.max;
    const int64_t bucket_start = BucketStart(first.min, request);
    if (alone && inside_range && bucket_start == BucketStart(first.max, request)) {
      // Push-down: nothing can overwrite this block and all of it belongs to one bucket,
      // so its stored statistics are the answer.
      Bucket summary;
      summary.count = first.block->count;
      summary.sum = first.block->sum;
      summary.min = first.block->min;
      summary.max = first.block->max;
      BucketFor(bucket_start, &out->buckets)->Merge(summary);
      ++out->stats.blocks_from_stats;
    } else {
      // Read the group's stretch of time from every source, which settles overwrites, and
      // put each point into its bucket.
      const TimeRange part{std::max(range.min, first.min), std::min(range.max, group_max)};
      Status s = QueryRange(files, memtable, request.series_id, part, &points);
      if (!s.ok()) {
        return s;
      }
      for (size_t k = 0; k < points.size(); ++k) {
        const int64_t start = BucketStart(points.timestamps[k], request);
        BucketFor(start, &out->buckets)->Add(points.values[k]);
      }
      for (size_t k = i; k < j; ++k) {
        if (spans[k].block != nullptr) {
          ++out->stats.blocks_decoded;
        }
      }
    }
    i = j;
  }
  return Status::OK();
}

}  // namespace tsengine
