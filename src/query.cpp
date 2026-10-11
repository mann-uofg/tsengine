#include "tsengine/query.h"

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

}  // namespace tsengine
