#include "tsengine/series_index.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "tsengine/line_protocol.h"

namespace tsengine {
namespace {

// Posting lists are keyed by a "term": a one-letter kind followed by what it stands for.
std::string Term(char kind, std::string_view name) {
  std::string term(1, kind);
  term += name;
  return term;
}

std::string TagTerm(std::string_view key, std::string_view value) {
  std::string term = Term('T', key);
  term += '=';
  term += value;
  return term;
}

std::vector<uint64_t> Union(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
  std::vector<uint64_t> out;
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

std::vector<uint64_t> Intersect(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
  std::vector<uint64_t> out;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

}  // namespace

void SeriesIndex::Add(uint64_t series_id, std::string_view key) {
  keys_.emplace(series_id, std::string(key));
  all_.push_back(series_id);
  SeriesKeyParts parts;
  if (!ParseSeriesKey(key, &parts).ok()) {
    return;
  }
  postings_[Term('M', parts.measurement)].push_back(series_id);
  postings_[Term('F', parts.field)].push_back(series_id);
  for (const auto& [tag, value] : parts.tags) {
    postings_[TagTerm(tag, value)].push_back(series_id);
  }
}

const std::vector<uint64_t>& SeriesIndex::Posting(const std::string& term) const {
  static const std::vector<uint64_t> kEmpty;
  const auto it = postings_.find(term);
  return it == postings_.end() ? kEmpty : it->second;
}

std::vector<uint64_t> SeriesIndex::Select(const SeriesSelector& selector) const {
  std::vector<uint64_t> result = all_;
  if (!selector.measurement.empty()) {
    result = Intersect(result, Posting(Term('M', selector.measurement)));
  }
  if (!selector.field.empty()) {
    result = Intersect(result, Posting(Term('F', selector.field)));
  }
  for (const TagFilter& filter : selector.tags) {
    std::vector<uint64_t> any;
    for (const std::string& value : filter.values) {
      any = Union(any, Posting(TagTerm(filter.key, value)));
    }
    result = Intersect(result, any);
  }
  return result;
}

std::string_view SeriesIndex::KeyOf(uint64_t series_id) const {
  const auto it = keys_.find(series_id);
  if (it == keys_.end()) {
    return std::string_view();
  }
  return it->second;
}

std::string SeriesIndex::TagValue(uint64_t series_id, std::string_view tag) const {
  SeriesKeyParts parts;
  if (!ParseSeriesKey(KeyOf(series_id), &parts).ok()) {
    return std::string();
  }
  for (const auto& [key, value] : parts.tags) {
    if (key == tag) {
      return std::string(value);
    }
  }
  return std::string();
}

}  // namespace tsengine
