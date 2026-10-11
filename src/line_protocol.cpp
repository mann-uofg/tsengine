#include "tsengine/line_protocol.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

namespace tsengine {
namespace {

// Index of the first character from `stops` at or after `pos` that is not escaped with a
// backslash, or s.size() if there is none.
size_t FindUnescaped(std::string_view s, size_t pos, std::string_view stops) {
  for (size_t i = pos; i < s.size(); ++i) {
    if (s[i] == '\\') {
      ++i;
      continue;
    }
    if (stops.find(s[i]) != std::string_view::npos) {
      return i;
    }
  }
  return s.size();
}

size_t SkipSpaces(std::string_view s, size_t pos) {
  while (pos < s.size() && s[pos] == ' ') {
    ++pos;
  }
  return pos;
}

// Parses `text` as a T. The whole string must be consumed.
template <typename T>
bool ParseWhole(std::string_view text, T* value) {
  const char* first = text.data();
  const char* last = first + text.size();
  if (first == last) {
    return false;
  }
  const auto [ptr, ec] = std::from_chars(first, last, *value);
  return ec == std::errc() && ptr == last;
}

Status ParseFieldValue(std::string_view text, double* value) {
  if (text.empty()) {
    return Status::InvalidArgument("field has no value");
  }
  if (text.front() == '"') {
    return Status::InvalidArgument("string fields are not supported");
  }
  const std::string_view digits = text.substr(0, text.size() - 1);
  if (text.back() == 'i') {
    int64_t v = 0;
    if (!ParseWhole(digits, &v)) {
      return Status::InvalidArgument("invalid integer field value");
    }
    *value = static_cast<double>(v);
    return Status::OK();
  }
  if (text.back() == 'u') {
    uint64_t v = 0;
    if (!ParseWhole(digits, &v)) {
      return Status::InvalidArgument("invalid unsigned field value");
    }
    *value = static_cast<double>(v);
    return Status::OK();
  }
  double v = 0.0;
  if (!ParseWhole(text, &v) || !std::isfinite(v)) {
    return Status::InvalidArgument("invalid or unsupported field value");
  }
  *value = v;
  return Status::OK();
}

// Parses the "measurement,tag1=a,tag2=b" that both a line and a series key start with, into
// out->measurement and out->tags. On return *end is the index of the space that follows it,
// or text.size() if nothing follows.
template <typename T>
Status ParseMeasurementAndTags(std::string_view text, T* out, size_t* end) {
  out->tags.clear();
  size_t pos = FindUnescaped(text, 0, ", ");
  if (pos == 0) {
    return Status::InvalidArgument("missing measurement");
  }
  out->measurement = text.substr(0, pos);

  while (pos < text.size() && text[pos] == ',') {
    const size_t key_begin = pos + 1;
    const size_t eq = FindUnescaped(text, key_begin, "=, ");
    if (eq >= text.size() || text[eq] != '=') {
      return Status::InvalidArgument("tag is missing '='");
    }
    const size_t value_begin = eq + 1;
    const size_t value_end = FindUnescaped(text, value_begin, ", ");
    if (eq == key_begin || value_end == value_begin) {
      return Status::InvalidArgument("tag has an empty key or value");
    }
    out->tags.emplace_back(text.substr(key_begin, eq - key_begin),
                           text.substr(value_begin, value_end - value_begin));
    pos = value_end;
  }
  *end = pos;
  return Status::OK();
}

}  // namespace

Status ParseLine(std::string_view line, ParsedLine* out) {
  out->fields.clear();
  out->has_timestamp = false;
  out->timestamp_ns = 0;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
    line.remove_suffix(1);
  }

  size_t pos = 0;
  Status s = ParseMeasurementAndTags(line, out, &pos);
  if (!s.ok()) {
    return s;
  }
  if (pos >= line.size()) {
    return Status::InvalidArgument("missing fields");
  }
  pos = SkipSpaces(line, pos);

  // Fields: one or more "key=value" separated by commas.
  while (true) {
    const size_t eq = FindUnescaped(line, pos, "=, ");
    if (eq >= line.size() || line[eq] != '=' || eq == pos) {
      return Status::InvalidArgument("field is missing a key or '='");
    }
    const size_t value_begin = eq + 1;
    size_t value_end = line.find_first_of(", ", value_begin);
    if (value_end == std::string_view::npos) {
      value_end = line.size();
    }
    LineField field;
    field.key = line.substr(pos, eq - pos);
    s = ParseFieldValue(line.substr(value_begin, value_end - value_begin), &field.value);
    if (!s.ok()) {
      return s;
    }
    out->fields.push_back(field);
    pos = value_end;
    if (pos >= line.size() || line[pos] != ',') {
      break;
    }
    ++pos;
  }

  // Optional timestamp in nanoseconds.
  pos = SkipSpaces(line, pos);
  if (pos < line.size()) {
    if (!ParseWhole(line.substr(pos), &out->timestamp_ns)) {
      return Status::InvalidArgument("invalid timestamp");
    }
    out->has_timestamp = true;
  }

  std::sort(out->tags.begin(), out->tags.end());
  for (size_t i = 1; i < out->tags.size(); ++i) {
    if (out->tags[i].first == out->tags[i - 1].first) {
      return Status::InvalidArgument("duplicate tag key");
    }
  }
  return Status::OK();
}

void AppendSeriesKey(const ParsedLine& line, std::string_view field_key, std::string* out) {
  out->append(line.measurement);
  for (const auto& [key, value] : line.tags) {
    out->push_back(',');
    out->append(key);
    out->push_back('=');
    out->append(value);
  }
  out->push_back(' ');
  out->append(field_key);
}

Status ParseSeriesKey(std::string_view key, SeriesKeyParts* out) {
  size_t pos = 0;
  Status s = ParseMeasurementAndTags(key, out, &pos);
  if (!s.ok()) {
    return s;
  }
  // Exactly one space, then the field name, which runs to the end of the key.
  if (pos + 1 >= key.size() || key[pos] != ' ') {
    return Status::InvalidArgument("series key has no field name");
  }
  out->field = key.substr(pos + 1);
  return Status::OK();
}

}  // namespace tsengine
