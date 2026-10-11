#pragma once

#include <cstdint>
#include <span>

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

}  // namespace tsengine
