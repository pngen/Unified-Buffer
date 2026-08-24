#pragma once
#include "unified_buffer/core/types.hpp"
#include "unified_buffer/core/capabilities.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace unified_buffer {

struct BackendInfo {
  BackendId id;
  std::string name;
  bool enabled = false;
  std::vector<MemoryDomain> domains;
  Capabilities capabilities;
};

struct DeviceInfo {
  BackendId backend;
  int index = -1;
  std::string name;
  std::uint64_t total_memory = 0;
  std::uint64_t free_memory = 0;
  int compute_major = 0;
  int compute_minor = 0;
  bool available = false;
};

struct DomainInfo {
  MemoryDomain domain;
  std::string name;
  BackendId backend;
  bool enabled = false;
  Capabilities capabilities;
  std::uint64_t total_capacity = 0;
  std::uint64_t free = 0;
};

struct RuntimeStats {
  std::string version;
  bool closed = false;
  std::uint64_t active_buffers = 0;
  std::uint64_t total_allocations = 0;
  std::uint64_t total_frees = 0;
  std::uint64_t failed_allocations = 0;
  std::uint64_t bytes_requested = 0;
  std::uint64_t bytes_committed = 0;
  std::uint64_t peak_bytes = 0;
  std::uint64_t host_bytes = 0;
  std::uint64_t pinned_host_bytes = 0;
  std::uint64_t device_bytes = 0;
  std::uint64_t shared_host_bytes = 0;
  std::uint64_t file_backed_bytes = 0;
  std::uint64_t pooled_bytes = 0;
  std::uint64_t idle_pooled_bytes = 0;
  std::uint64_t pool_hits = 0;
  std::uint64_t pool_misses = 0;
  std::uint64_t pool_evictions = 0;
  std::uint64_t reservations = 0;
  std::uint64_t reservation_failures = 0;
  std::uint64_t map_count = 0;
  std::uint64_t unmap_count = 0;
  std::uint64_t export_count = 0;
  std::uint64_t import_count = 0;
  std::uint64_t copy_operations = 0;
  std::uint64_t bytes_copied = 0;
  std::uint64_t migration_count = 0;
  std::uint64_t zero_operations = 0;
  std::uint64_t integrity_checks = 0;
  std::uint64_t integrity_failures = 0;
  std::uint64_t stale_handle_rejections = 0;
  std::uint64_t stale_generation_rejections = 0;
  std::uint64_t quota_rejections = 0;
  std::uint64_t lease_conflicts = 0;
  std::uint64_t outstanding_allocations = 0;
  std::vector<DomainInfo> domains;
};

} // namespace unified_buffer
