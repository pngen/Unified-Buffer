#include "internal.hpp"
#include "unified_buffer/runtime.hpp"
#include "unified_buffer/telemetry.hpp"
#include "unified_buffer/version.hpp"

namespace unified_buffer {
namespace internal {

Result<RuntimeStats> RuntimeImpl::runtime_stats() const {
  RuntimeStats st;
  st.version = version_string();
  st.closed = closed();
  auto t = telemetry();
  st.active_buffers = t.active_buffers;
  st.total_allocations = t.total_allocations;
  st.total_frees = t.total_frees;
  st.failed_allocations = t.failed_allocations;
  st.bytes_requested = t.bytes_requested;
  st.bytes_committed = t.bytes_committed;
  st.peak_bytes = t.peak_bytes;
  st.host_bytes = t.host_bytes;
  st.pinned_host_bytes = t.pinned_host_bytes;
  st.device_bytes = t.device_bytes;
  st.shared_host_bytes = t.shared_host_bytes;
  st.file_backed_bytes = t.file_backed_bytes;
  st.pooled_bytes = t.pooled_bytes;
  st.idle_pooled_bytes = t.idle_pooled_bytes;
  st.pool_hits = t.pool_hits;
  st.pool_misses = t.pool_misses;
  st.pool_evictions = t.pool_evictions;
  st.reservations = t.reservations;
  st.reservation_failures = t.reservation_failures;
  st.map_count = t.map_count;
  st.unmap_count = t.unmap_count;
  st.export_count = t.export_count;
  st.import_count = t.import_count;
  st.copy_operations = t.copy_count;
  st.bytes_copied = t.bytes_copied;
  st.migration_count = t.migration_count;
  st.zero_operations = t.zero_count;
  st.integrity_checks = t.integrity_checks;
  st.integrity_failures = t.integrity_failures;
  st.stale_handle_rejections = t.stale_handle_rejections;
  st.stale_generation_rejections = t.stale_generation_rejections;
  st.quota_rejections = t.quota_rejections;
  st.lease_conflicts = t.lease_conflicts;
  st.outstanding_allocations = t.outstanding_allocations;
  st.domains = domains_info().value_or({});
  return ok(std::move(st));
}

Result<std::vector<BackendInfo>> RuntimeImpl::backends_info() const {
  std::vector<BackendInfo> out;
  auto add = [&](BackendId id, bool enabled, std::vector<MemoryDomain> doms) {
    BackendInfo bi;
    bi.id = id;
    bi.name = std::string(to_string(id));
    bi.enabled = enabled;
    bi.domains = std::move(doms);
    for (auto d : bi.domains) if (domain(d)) bi.capabilities = domain_caps(d);
    out.push_back(bi);
  };
  add(BackendId::HOST_MALLOC, domain_enabled(MemoryDomain::HOST), {MemoryDomain::HOST});
  add(BackendId::CUDA, domain_enabled(MemoryDomain::DEVICE), {MemoryDomain::DEVICE, MemoryDomain::PINNED_HOST});
  add(BackendId::WIN_SHARED_FILE, domain_enabled(MemoryDomain::SHARED_HOST), {MemoryDomain::SHARED_HOST});
  add(BackendId::FILE_BACKED, domain_enabled(MemoryDomain::MMAP_STORAGE), {MemoryDomain::MMAP_STORAGE});
  return ok(std::move(out));
}

Result<std::vector<DeviceInfo>> RuntimeImpl::devices_info() const {
  std::vector<DeviceInfo> out;
#ifdef UNIFIED_BUFFER_HAS_CUDA
  if (cuda_support::available()) {
    int n = cuda_support::device_count();
    for (int i = 0; i < n; ++i) {
      DeviceInfo di;
      di.backend = BackendId::CUDA;
      di.index = i;
      auto info = cuda_support::device_info(i);
      if (info.ok()) {
        di.name = info.value().name;
        di.total_memory = info.value().total_memory;
        di.free_memory = info.value().free_memory;
        di.compute_major = info.value().compute_major;
        di.compute_minor = info.value().compute_minor;
        di.available = true;
      }
      out.push_back(di);
    }
  }
#endif
  return ok(std::move(out));
}

Result<std::vector<DomainInfo>> RuntimeImpl::domains_info() const {
  std::vector<DomainInfo> out;
  auto add = [&](MemoryDomain d) {
    DomainInfo di;
    di.domain = d;
    di.name = std::string(to_string(d));
    auto* dom = domain(d);
    di.enabled = dom != nullptr;
    if (dom) {
      di.backend = dom->backend();
      di.capabilities = dom->capabilities();
      auto cap = dom->capacity();
      if (cap.ok()) { di.total_capacity = cap.value().total_capacity; di.free = cap.value().free; }
    }
    out.push_back(di);
  };
  add(MemoryDomain::HOST);
  add(MemoryDomain::PINNED_HOST);
  add(MemoryDomain::DEVICE);
  add(MemoryDomain::SHARED_HOST);
  add(MemoryDomain::MMAP_STORAGE);
  return ok(std::move(out));
}

}  // namespace internal
}