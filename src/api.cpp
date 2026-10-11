#include "tsengine/api.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <span>
#include <unordered_map>

#include "tsengine/query.h"

namespace tsengine {
namespace {

// A WAL record holds at most 16 MiB, which is about 699,000 samples.
constexpr size_t kMaxSamplesPerBatch = 500000;

// Lets a map with std::string keys be searched with a string_view.
struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); }
};

void AppendJsonString(std::string_view text, std::string* out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(c);
    } else if (byte < 0x20) {
      out->append("\\u00");
      out->push_back(kHex[byte >> 4]);
      out->push_back(kHex[byte & 0xF]);
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
}

void AppendInt(int64_t value, std::string* out) { out->append(std::to_string(value)); }

void AppendUnsigned(uint64_t value, std::string* out) { out->append(std::to_string(value)); }

// Shortest text that reads back as exactly the same double. JSON has no NaN or infinity.
void AppendDouble(double value, std::string* out) {
  if (!std::isfinite(value)) {
    out->append("null");
    return;
  }
  char buf[32];
  const std::to_chars_result result = std::to_chars(buf, buf + sizeof(buf), value);
  out->append(buf, static_cast<size_t>(result.ptr - buf));
}

void AppendResult(std::string_view name, const Points& points, std::string* out) {
  out->append("{\"name\":");
  AppendJsonString(name, out);
  out->append(",\"timestamps\":[");
  for (size_t i = 0; i < points.size(); ++i) {
    if (i != 0) {
      out->push_back(',');
    }
    AppendInt(points.timestamps[i], out);
  }
  out->append("],\"values\":[");
  for (size_t i = 0; i < points.size(); ++i) {
    if (i != 0) {
      out->push_back(',');
    }
    AppendDouble(points.values[i], out);
  }
  out->append("]}");
}

double ValueOf(const Bucket& bucket, AggregateKind kind) {
  switch (kind) {
    case AggregateKind::kCount:
      return static_cast<double>(bucket.count);
    case AggregateKind::kSum:
      return bucket.sum;
    case AggregateKind::kMin:
      return bucket.min;
    case AggregateKind::kMax:
      return bucket.max;
    case AggregateKind::kMean:
      return bucket.sum / static_cast<double>(bucket.count);
    case AggregateKind::kNone:
      break;
  }
  return 0.0;
}

}  // namespace

bool ParseAggregateKind(std::string_view text, AggregateKind* kind) {
  if (text.empty()) {
    *kind = AggregateKind::kNone;
  } else if (text == "count") {
    *kind = AggregateKind::kCount;
  } else if (text == "sum") {
    *kind = AggregateKind::kSum;
  } else if (text == "min") {
    *kind = AggregateKind::kMin;
  } else if (text == "max") {
    *kind = AggregateKind::kMax;
  } else if (text == "mean" || text == "avg") {
    *kind = AggregateKind::kMean;
  } else {
    return false;
  }
  return true;
}

Status Api::Write(const WriteRequest& request, WriteSummary* summary) {
  ParsedBatch batch;
  Status s = Parse(request, &batch);
  if (!s.ok()) {
    *summary = WriteSummary();
    return s;
  }
  return Store(batch, summary);
}

Status Api::Parse(const WriteRequest& request, ParsedBatch* batch) {
  batch->keys.clear();
  batch->points.clear();
  batch->lines = 0;
  // Series key -> its position in batch->keys. A body usually repeats the same few series
  // many times, so each distinct key is stored once.
  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> key_index;
  ParsedLine line;
  std::string key;
  const std::string_view body = request.body;
  uint64_t line_number = 0;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    if (end == std::string_view::npos) {
      end = body.size();
    }
    std::string_view text = body.substr(pos, end - pos);
    pos = end + 1;
    ++line_number;
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
      text.remove_suffix(1);
    }
    if (text.empty() || text.front() == '#') {
      continue;
    }

    Status s = ParseLine(text, &line);
    if (!s.ok()) {
      return Status::InvalidArgument("line " + std::to_string(line_number) + ": " + s.message());
    }
    int64_t timestamp = request.now_ns;
    if (line.has_timestamp) {
      timestamp = line.timestamp_ns * request.timestamp_unit_ns;
    }
    // Every field of the line is a series of its own.
    for (const LineField& field : line.fields) {
      key.clear();
      AppendSeriesKey(line, field.key, &key);
      auto it = key_index.find(std::string_view(key));
      if (it == key_index.end()) {
        it = key_index.emplace(key, static_cast<uint32_t>(batch->keys.size())).first;
        batch->keys.push_back(key);
      }
      batch->points.push_back(ParsedBatch::Point{it->second, timestamp, field.value});
    }
    ++batch->lines;
  }
  return Status::OK();
}

Status Api::Store(const ParsedBatch& batch, WriteSummary* summary) {
  *summary = WriteSummary();
  // Resolve each distinct series once, then turn the points into samples.
  ids_.clear();
  for (const std::string& key : batch.keys) {
    uint64_t id = 0;
    Status s = engine_->GetOrCreateSeries(key, &id);
    if (!s.ok()) {
      return s;
    }
    ids_.push_back(id);
  }
  samples_.clear();
  samples_.reserve(batch.points.size());
  for (const ParsedBatch::Point& point : batch.points) {
    samples_.push_back(Sample{ids_[point.key], point.timestamp_ns, point.value});
  }

  const std::span<const Sample> all(samples_);
  for (size_t begin = 0; begin < all.size(); begin += kMaxSamplesPerBatch) {
    const size_t n = std::min(kMaxSamplesPerBatch, all.size() - begin);
    Status s = engine_->Write(all.subspan(begin, n));
    if (!s.ok()) {
      return s;
    }
  }
  summary->lines = batch.lines;
  summary->points = samples_.size();
  return Status::OK();
}

Status Api::Query(const QueryRequest& request, std::string* json) {
  const SeriesIndex& index = engine_->series();
  const std::vector<uint64_t> ids = index.Select(request.selector);
  AggregateStats stats;
  json->assign("{\"results\":[");
  bool first = true;

  if (request.aggregate == AggregateKind::kNone) {
    Points points;
    for (const uint64_t id : ids) {
      Status s = engine_->Query(id, request.range, &points);
      if (!s.ok()) {
        return s;
      }
      if (points.empty()) {
        continue;
      }
      if (!first) {
        json->push_back(',');
      }
      first = false;
      AppendResult(index.KeyOf(id), points, json);
    }
  } else {
    // Group name -> bucket start -> the buckets of every series of the group, combined.
    std::map<std::string, std::map<int64_t, Bucket>> groups;
    AggregateRequest aggregate;
    aggregate.range = request.range;
    aggregate.bucket_width = request.bucket_width_ns;
    AggregateResult result;
    for (const uint64_t id : ids) {
      aggregate.series_id = id;
      Status s = engine_->Aggregate(aggregate, &result);
      if (!s.ok()) {
        return s;
      }
      if (result.buckets.empty()) {
        continue;
      }
      std::string group;
      if (request.group_by == kGroupByField) {
        SeriesKeyParts parts;
        if (ParseSeriesKey(index.KeyOf(id), &parts).ok()) {
          group = parts.field;
        }
      } else if (!request.group_by.empty()) {
        group = index.TagValue(id, request.group_by);
      }
      std::map<int64_t, Bucket>& combined = groups[group];
      for (const Bucket& bucket : result.buckets) {
        Bucket& target = combined[bucket.start];
        target.start = bucket.start;
        target.Merge(bucket);
      }
    }
    stats = result.stats;

    Points points;
    for (const auto& [name, buckets] : groups) {
      points.clear();
      for (const auto& entry : buckets) {
        points.Append(entry.first, ValueOf(entry.second, request.aggregate));
      }
      if (!first) {
        json->push_back(',');
      }
      first = false;
      AppendResult(name, points, json);
    }
  }

  json->append("],\"stats\":{\"series\":");
  AppendUnsigned(ids.size(), json);
  json->append(",\"blocks_from_stats\":");
  AppendUnsigned(stats.blocks_from_stats, json);
  json->append(",\"blocks_decoded\":");
  AppendUnsigned(stats.blocks_decoded, json);
  json->append("}}");
  return Status::OK();
}

std::string Api::StatsJson() const {
  std::string json = "{\"series\":";
  AppendUnsigned(engine_->series_count(), &json);
  json += ",\"block_files\":";
  AppendUnsigned(engine_->Files().size(), &json);
  json += ",\"memtable_points\":";
  AppendUnsigned(engine_->memtable_points(), &json);
  json += ",\"wal_segment\":";
  AppendUnsigned(engine_->wal_segment(), &json);
  json += "}";
  return json;
}

std::string Api::ErrorJson(const Status& status) {
  std::string json = "{\"error\":";
  AppendJsonString(status.ToString(), &json);
  json += "}";
  return json;
}

}  // namespace tsengine
