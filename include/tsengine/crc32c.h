#pragma once

#include <cstddef>
#include <cstdint>

namespace tsengine {

// CRC-32C (Castagnoli polynomial), the checksum used by iSCSI, ext4, LevelDB and RocksDB.
//
// Crc32cExtend continues a checksum over more bytes, so a checksum can be built in pieces:
// Crc32cExtend(Crc32c(a, n), b, m) equals the checksum of a followed by b.
//
// Uses the processor's CRC32C instruction where there is one, and a lookup table otherwise.
uint32_t Crc32cExtend(uint32_t crc, const void* data, size_t n);

// The lookup-table implementation, whatever the processor supports. Exists so that tests can
// check the two implementations against each other.
uint32_t Crc32cExtendPortable(uint32_t crc, const void* data, size_t n);

inline uint32_t Crc32c(const void* data, size_t n) { return Crc32cExtend(0, data, n); }

}  // namespace tsengine
