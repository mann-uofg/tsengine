#include "tsengine/memtable.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "tsengine/block_file.h"

namespace tsengine {

void Memtable::Append(uint64_t series_id, int64_t timestamp_ns, double value) {
  Series& series = series_[series_id];
  const std::vector<int64_t>& timestamps = series.points.timestamps;
  if (!timestamps.empty() && timestamp_ns <= timestamps.back()) {
    series.ordered = false;
  }
  series.points.Append(timestamp_ns, value);
  ++point_count_;
}

std::vector<uint64_t> Memtable::SeriesIds() const {
  std::vector<uint64_t> ids;
  ids.reserve(series_.size());
  for (const auto& entry : series_) {
    ids.push_back(entry.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

void Memtable::ReadRange(uint64_t series_id, TimeRange range, Points* out) const {
  const auto it = series_.find(series_id);
  if (it == series_.end()) {
    return;
  }
  const Series& series = it->second;
  if (series.ordered) {
    AppendRange(series.points, range, out);
    return;
  }

  // Out-of-order or repeated timestamps. Sort (timestamp, arrival position) pairs: equal
  // timestamps then sit next to each other in arrival order, so the last of each run is the
  // newest write.
  const std::vector<int64_t>& timestamps = series.points.timestamps;
  std::vector<std::pair<int64_t, size_t>> order;
  for (size_t i = 0; i < timestamps.size(); ++i) {
    if (range.Contains(timestamps[i])) {
      order.emplace_back(timestamps[i], i);
    }
  }
  std::sort(order.begin(), order.end());
  for (size_t i = 0; i < order.size(); ++i) {
    const bool overwritten = i + 1 < order.size() && order[i + 1].first == order[i].first;
    if (!overwritten) {
      out->Append(order[i].first, series.points.values[order[i].second]);
    }
  }
}

Status FlushMemtable(const Memtable& memtable, const std::string& path) {
  std::unique_ptr<BlockFileWriter> writer;
  Status s = BlockFileWriter::Create(path, &writer);
  if (!s.ok()) {
    return s;
  }
  Points points;
  for (const uint64_t series_id : memtable.SeriesIds()) {
    points.clear();
    memtable.ReadRange(series_id, TimeRange(), &points);
    s = writer->AddSeries(series_id, points);
    if (!s.ok()) {
      return s;
    }
  }
  return writer->Finish();
}

}  // namespace tsengine
