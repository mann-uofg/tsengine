#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tsengine {

// Matches series whose tag `key` has any one of `values`.
struct TagFilter {
  std::string key;
  std::vector<std::string> values;
};

// Describes a set of series. An empty measurement or field matches any. Every tag filter
// must match.
struct SeriesSelector {
  std::string measurement;
  std::string field;
  std::vector<TagFilter> tags;
};

// An inverted index over series keys: for every measurement, field and tag=value pair it
// keeps the sorted list of series ids that have it (a posting list). Selecting series is
// then a matter of intersecting a few sorted lists, never of scanning every key.
//
// The index lives in memory and is rebuilt from the series log on startup.
class SeriesIndex {
 public:
  // Adds a series. Ids must be added in increasing order, which keeps every posting list
  // sorted without any extra work. A key that is not in canonical form is still recorded,
  // but can only be found by id.
  void Add(uint64_t series_id, std::string_view key);

  // Ids of the series that match `selector`, in increasing order.
  std::vector<uint64_t> Select(const SeriesSelector& selector) const;

  // The key of a series, or an empty string if the id is unknown.
  std::string_view KeyOf(uint64_t series_id) const;

  // The value of one tag of a series, or an empty string if the series does not have it.
  std::string TagValue(uint64_t series_id, std::string_view tag) const;

  size_t size() const { return all_.size(); }

 private:
  const std::vector<uint64_t>& Posting(const std::string& term) const;

  std::unordered_map<uint64_t, std::string> keys_;
  std::unordered_map<std::string, std::vector<uint64_t>> postings_;
  std::vector<uint64_t> all_;
};

}  // namespace tsengine
