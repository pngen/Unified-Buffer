#pragma once
#include "unified_buffer/core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace unified_buffer {

// Bounded pool configuration.
struct PoolConfig {
  bool enabled = true;
  std::uint64_t max_idle_bytes = 256ULL * 1024 * 1024;  // 256 MiB of idle pooled memory
  std::uint64_t max_objects = 4096;       // max pooled buffer objects
  std::vector<std::uint64_t> size_classes;  // if empty, classes are derived
  // Minimum & maximum size-class exponent window (for derived classes).
  std::uint64_t min_class_size = 4096;
  std::uint64_t max_class_size = 256ULL * 1024 * 1024;
};

// Per-namespace quota and policy.
struct NamespaceConfig {
  std::string name;
  std::uint64_t host_quota = 0;         // 0 = unlimited
  std::uint64_t pinned_quota = 0;
  std::uint64_t device_quota = 0;
  std::uint64_t shared_quota = 0;
  std::uint64_t file_quota = 0;
  std::uint64_t allocation_count_quota = 0;
  ZeroingPolicy zeroing = ZeroingPolicy::ON_CROSS_OWNER_REUSE;
  bool allow_export = true;
  bool allow_import = true;
  int priority = 0;
};

// Runtime configuration.
struct RuntimeConfig {
  std::string name = "unified_buffer";
  bool enable_host = true;
  bool enable_shared = true;
  bool enable_file = true;
  bool enable_pool = true;
  PoolConfig pool;
  std::vector<NamespaceConfig> namespaces;
  // 0 = auto-detect host capacity.
  std::uint64_t host_capacity_hint = 0;
  // Per-domain capacity caps (0 = backend-reported / default).
  std::uint64_t host_cap = 0;
  std::uint64_t pinned_cap = 0;
  std::uint64_t device_cap = 0;
  std::uint64_t shared_cap = 0;
  std::uint64_t file_cap = 0;
};

} // namespace unified_buffer
