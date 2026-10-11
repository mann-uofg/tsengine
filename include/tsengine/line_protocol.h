#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tsengine/status.h"

namespace tsengine {

// Parser for Influx line protocol:
//
//   measurement,tag1=a,tag2=b field1=1.5,field2=3i 1451606400000000000
//
// Supported field values are floats, signed integers (suffix i) and unsigned integers
// (suffix u); all are stored as float64. String and boolean fields are rejected.

struct LineField {
  std::string_view key;
  double value = 0.0;
};

// The pieces of one line. Every string_view points into the input line, so the line must
// outlive this struct. Names are kept exactly as written, backslash escapes included.
struct ParsedLine {
  std::string_view measurement;
  // Sorted by key, so the same tag set always produces the same series key.
  std::vector<std::pair<std::string_view, std::string_view>> tags;
  std::vector<LineField> fields;
  bool has_timestamp = false;
  int64_t timestamp_ns = 0;
};

// Parses a single line (without its newline). Blank lines and comments are the caller's job.
// `out` is overwritten and may be reused across calls to avoid reallocating its vectors.
Status ParseLine(std::string_view line, ParsedLine* out);

// Appends the canonical key of the series that one field of `line` belongs to:
//
//   measurement,tag1=a,tag2=b field
//
// Each field of a line is its own series, so a line with N fields has N series keys.
void AppendSeriesKey(const ParsedLine& line, std::string_view field_key, std::string* out);

// The pieces of a canonical series key. The string_views point into the key.
struct SeriesKeyParts {
  std::string_view measurement;
  std::vector<std::pair<std::string_view, std::string_view>> tags;
  std::string_view field;
};

// Splits a key produced by AppendSeriesKey back into its measurement, tags and field.
Status ParseSeriesKey(std::string_view key, SeriesKeyParts* out);

}  // namespace tsengine
