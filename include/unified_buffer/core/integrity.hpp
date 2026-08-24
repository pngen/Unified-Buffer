#pragma once
#include <cstdint>
#include <cstddef>

namespace unified_buffer {

// CRC-32C (Castagnoli).  A fast content-integrity checksum used for explicit
// verification.  Not a cryptographic hash; for durable/exported anchors the
// runtime also stores metadata checksums.

// Returns the CRC-32C of `data` seeded with `seed`.  Pass 0 for a fresh hash.
std::uint32_t crc32c(const void* data, std::size_t len, std::uint32_t seed = 0);

// Static self-check used by tests: verifies the table against the known
// CRC-32C("123456789") == 0xE3069283.
bool crc32c_self_test();

} // namespace unified_buffer
