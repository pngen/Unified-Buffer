#pragma once
#include <cstdint>
#include <functional>

namespace unified_buffer {

// 128-bit stable logical buffer identifier.  Addresses may change; identity
// must not.  BufferId is the primary handle-independent identity of a buffer.
struct BufferId {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  bool operator==(const BufferId& o) const noexcept { return hi == o.hi && lo == o.lo; }
  bool operator!=(const BufferId& o) const noexcept { return !(*this == o); }
  bool operator<(const BufferId& o) const noexcept {
    return hi < o.hi || (hi == o.hi && lo < o.lo);
  }
  [[nodiscard]] bool null() const noexcept { return hi == 0 && lo == 0; }
};

// Monotonic per-lineage generation.  Generation advances on operations that
// invalidate prior handles or authoritative assumptions.
using BufferGeneration = std::uint64_t;

// Namespace identity.  Namespaces isolate capacity, quotas, and policies.
using NamespaceId = std::uint64_t;

// A default / null namespace id.
inline constexpr NamespaceId kDefaultNamespace = 1;

} // namespace unified_buffer

// std::hash specialization for use in unordered containers.
template <>
struct std::hash<unified_buffer::BufferId> {
  std::size_t operator()(const unified_buffer::BufferId& id) const noexcept {
    // splitmix64 style mixing for a decent 64-bit hash of the 128-bit id.
    std::uint64_t x = id.hi ^ (id.lo + 0x9e3779b97f4a7c15ULL + (id.hi << 6) + (id.hi >> 2));
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return static_cast<std::size_t>(x);
  }
};

