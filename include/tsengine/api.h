#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tsengine/engine.h"
#include "tsengine/line_protocol.h"
#include "tsengine/series_index.h"
#include "tsengine/status.h"
#include "tsengine/types.h"
#include "tsengine/wal.h"

namespace tsengine {

// What the outside world can ask of the engine, independent of HTTP: write a body of line
// protocol, and run a query that returns JSON. The HTTP server is a thin wrapper over this,
// which keeps all of it testable without a socket.

enum class AggregateKind { kNone, kCount, kSum, kMin, kMax, kMean };

// Accepts "", "count", "sum", "min", "max" and "mean" (or "avg"). "" means no aggregation.
bool ParseAggregateKind(std::string_view text, AggregateKind* kind);

struct WriteRequest {
  // Line protocol, one point per line. Blank lines and lines starting with # are skipped.
  std::string_view body;
  // What one unit of the timestamps in the body is worth: 1 for nanoseconds, 1000000000 for
  // seconds, and so on.
  int64_t timestamp_unit_ns = 1;
  // Timestamp given to lines that carry none.
  int64_t now_ns = 0;
};

struct WriteSummary {
  uint64_t lines = 0;
  uint64_t points = 0;
};

struct QueryRequest {
  SeriesSelector selector;
  TimeRange range;
  // kNone returns the raw points of every matching series.
  AggregateKind aggregate = AggregateKind::kNone;
  // Width of the time buckets of an aggregate. 0 means one bucket for the whole range.
  int64_t bucket_width_ns = 0;
  // For aggregates: a tag name. Series are combined per value of this tag. Empty combines
  // all matching series into one result.
  std::string group_by;
};

class Api {
 public:
  explicit Api(Engine* engine) : engine_(engine) {}

  // Parses and stores a body of line protocol. If any line is malformed, nothing is stored
  // and the error names the line. A very large body is stored as several WAL batches.
  Status Write(const WriteRequest& request, WriteSummary* summary);

  // Runs a query and writes the result as JSON:
  //
  //   {"results":[{"name":"...","timestamps":[...],"values":[...]}, ...],
  //    "stats":{"series":2,"blocks_from_stats":10,"blocks_decoded":1}}
  //
  // For raw queries a result is one series and its name is the series key. For aggregates
  // a result is one group and its name is the group's tag value.
  Status Query(const QueryRequest& request, std::string* json);

  Status Flush() { return engine_->Flush(); }
  Status Compact() { return engine_->Compact(); }

  // {"series":N,"block_files":N,"memtable_points":N,"wal_segment":N}
  std::string StatsJson() const;

  // {"error":"..."}
  static std::string ErrorJson(const Status& status);

 private:
  Engine* engine_;
  // Reused between calls so a write does not allocate per line.
  ParsedLine line_;
  std::string key_;
  std::vector<Sample> samples_;
};

}  // namespace tsengine
