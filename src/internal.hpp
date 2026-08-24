#pragma once
#include "unified_buffer/backends/domain.hpp"
#include "unified_buffer/backends/host_backend.hpp"
#include "unified_buffer/backends/cuda_backend.hpp"
#include "unified_buffer/core/identity.hpp"
#include "unified_buffer/core/types.hpp"
#include "unified_buffer/config.hpp"
#include "unified_buffer/core/result.hpp"
#include "unified_buffer/core/checked_math.hpp"
#include "unified_buffer/core/integrity.hpp"
#include "unified_buffer/export.hpp"
#include "unified_buffer/telemetry.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace unified_buffer {
struct AllocationRequest;
struct ExternalMemoryDesc;
class Runtime;
class BufferHandle;

namespace internal {

class RuntimeImpl;

// Buffer metadata record.
struct Record {
  BufferId id;
  BufferGeneration generation = 0;
  NamespaceId ns = kDefaultNamespace;
  MemoryDomain domain = MemoryDomain::HOST;
  BackendId backend = BackendId::HOST_MALLOC;
  DeviceId device;
  std::uint64_t size = 0;
  std::uint64_t committed = 0;
  std::uint64_t alignment = 0;
  AllocationFlags flags;
  AccessMode access = AccessMode::READ_WRITE;
  bool pool_eligible = false;
  Ownership ownership = Ownership::RUNTIME;
  BufferState state = BufferState::DECLARED;
  NativeAllocation backing;
  bool pooled = false;
  bool exported = false;
  int map_count = 0;
  std::uint64_t created_epoch = 0;
  std::string label;
  std::uint32_t meta_crc = 0;
};

// Per-buffer mutable metadata.  `mtx` serializes mutation of a single buffer.
// `refs` counts every external reference (BufferHandle holders plus active
// leases/maps/exports); the buffer is finalized once when refs -> 0.
struct Control : std::enable_shared_from_this<Control> {
  std::shared_ptr<RuntimeImpl> rt;
  std::mutex mtx;
  Record rec;
  std::atomic<int> refs{0};
  std::atomic<bool> finalized{false};
  int read_leases = 0;
  int write_leases = 0;
  int exclusive_leases = 0;
  bool pool_eligible = false;
};

// A pooled NativeAllocation awaiting reuse.
struct PoolSlot {
  NativeAllocation backing;
  BufferId original_id;
  NamespaceId last_owner;
  bool zeroed = false;
};

// Pool key: compatibility dimensions.
struct PoolKey {
  MemoryDomain domain;
  BackendId backend;
  DeviceId device;
  std::uint64_t size_class;
  std::uint64_t alignment;
  bool exportable;
  bool operator==(const PoolKey& o) const noexcept {
    return domain == o.domain && backend == o.backend && device == o.device &&
           size_class == o.size_class && alignment == o.alignment &&
           exportable == o.exportable;
  }
};

struct PoolKeyHash {
  std::size_t operator()(const PoolKey& k) const noexcept {
    std::size_t h = std::hash<int>{}(static_cast<int>(k.domain));
    h = h * 31 + std::hash<int>{}(static_cast<int>(k.backend));
    h = h * 31 + std::hash<int>{}(k.device.index);
    h = h * 31 + std::hash<int>{}(static_cast<int>(k.device.backend));
    h = h * 31 + std::hash<std::uint64_t>{}(k.size_class);
    h = h * 31 + std::hash<std::uint64_t>{}(k.alignment);
    h = h * 31 + std::hash<int>{}(k.exportable ? 1 : 0);
    return h;
  }
};

std::uint64_t now_epoch_ms();
std::uint32_t meta_crc_of(const Record& r);
Status copy_memory(void* dst, const void* src, std::uint64_t len, bool dst_is_device, bool src_is_device);


}  // namespace internal

}  // namespace unified_buffer

namespace unified_buffer {
namespace internal {

// The shared implementation of the Runtime.  BufferHandle holds a shared_ptr to
// this so the runtime stays alive (but marked closed after shutdown) while any
// handle exists.
class RuntimeImpl : public std::enable_shared_from_this<RuntimeImpl> {
 public:
  explicit RuntimeImpl(const RuntimeConfig& cfg);
  ~RuntimeImpl();

  Result<Record> allocate_record(const AllocationRequest& req);
  Result<ExportDescriptor> export_descriptor(const Control& ctl);
  Result<std::shared_ptr<Control>> import_descriptor(const ExportDescriptor& desc, NamespaceId ns, const std::string& label);
  Result<RuntimeStats> runtime_stats() const;
  Result<std::vector<BackendInfo>> backends_info() const;
  Result<std::vector<DeviceInfo>> devices_info() const;
  Result<std::vector<DomainInfo>> domains_info() const;
  std::shared_ptr<Control> register_control(Record rec);
  Result<std::shared_ptr<Control>> wrap_external(const ExternalMemoryDesc& desc);
  Result<std::shared_ptr<Control>> migrate(const std::shared_ptr<Control>& src, MemoryDomain target);
  void retain(Control* c);
  void drop_ref(Control* c);
  void finalize(Control* c, bool return_to_pool);
  Status finalize_by_id(BufferId id);
  Result<std::shared_ptr<Control>> lookup(BufferId id, BufferGeneration expect_gen);

  std::optional<PoolSlot> pool_take(const PoolKey& key, NamespaceId owner, std::uint64_t min_committed);
  bool pool_put(PoolKey key, PoolSlot slot);
  Result<std::size_t> pool_size() const;
  Result<std::uint64_t> pool_bytes() const;
  Status pool_trim();
  void pool_trim_to_zero();

  IDomain* domain(MemoryDomain d) const;
  bool domain_enabled(MemoryDomain d) const;
  const Capabilities& domain_caps(MemoryDomain d) const;
  Result<std::uint64_t> capacity_for(MemoryDomain d) const;

  Status create_namespace(const NamespaceConfig& cfg);
  std::optional<NamespaceConfig> namespace_info(NamespaceId id) const;
  Status reserve_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes);
  Status commit_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes);
  void unreserve_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes);
  void checkout_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes);
  void release_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes, bool pooled);
  void free_accounting(NamespaceId ns, MemoryDomain d, std::uint64_t bytes);
  Status reserve_count(NamespaceId ns);
  void release_count(NamespaceId ns);

  bool valid_transition(BufferState from, BufferState to);
  Status set_state(Control* ctl, BufferState to);
  Status transition(Control* ctl, BufferState to);

  struct Telemetry {
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
    std::uint64_t reservations = 0;
    std::uint64_t reservation_failures = 0;
    std::uint64_t pool_hits = 0;
    std::uint64_t pool_misses = 0;
    std::uint64_t pool_evictions = 0;
    std::uint64_t pooled_bytes = 0;
    std::uint64_t idle_pooled_bytes = 0;
    std::uint64_t export_count = 0;
    std::uint64_t import_count = 0;
    std::uint64_t copy_count = 0;
    std::uint64_t bytes_copied = 0;
    std::uint64_t migration_count = 0;
    std::uint64_t zero_count = 0;
    std::uint64_t integrity_checks = 0;
    std::uint64_t integrity_failures = 0;
    std::uint64_t stale_handle_rejections = 0;
    std::uint64_t stale_generation_rejections = 0;
    std::uint64_t quota_rejections = 0;
    std::uint64_t lease_conflicts = 0;
    std::uint64_t outstanding_allocations = 0;
    std::uint64_t map_count = 0;
    std::uint64_t unmap_count = 0;
  };
  Telemetry telemetry() const;
  void note(const char* event, const std::string& detail);
  std::vector<std::pair<std::string, std::string>> decisions() const;

  bool closed() const { return closed_.load(); }
  void shutdown();
  const RuntimeConfig& config() const { return cfg; }
  std::shared_ptr<RuntimeImpl> shared() { return shared_from_this(); }
  Telemetry& counters() { return telem_; }
  void attach_shared_and_file_backends();

 private:
  BufferId next_id();
  std::uint64_t size_class_for(std::uint64_t bytes) const;
  PoolKey make_pool_key(const Record& rec) const;
  std::uint64_t cap_for(MemoryDomain d) const;
  std::uint64_t config_cap(MemoryDomain d) const;
  std::uint64_t domain_quota(NamespaceId ns, MemoryDomain d) const;
  void update_peak(std::uint64_t committed);
  Status validate_request(const AllocationRequest& req, std::uint64_t& alignment, std::uint64_t& class_size);

  RuntimeConfig cfg;
  std::atomic<bool> closed_{false};
  mutable std::shared_mutex registry_mtx_;
  std::unordered_map<BufferId, std::shared_ptr<Control>> registry_;
  mutable std::mutex pool_mtx_;
  std::unordered_map<PoolKey, std::deque<PoolSlot>, PoolKeyHash> pool_;
  std::uint64_t pool_idle_bytes_ = 0;
  std::uint64_t pool_object_count_ = 0;
  std::unique_ptr<HostDomain> host_;
  std::unique_ptr<IDomain> cuda_device_;
  std::unique_ptr<IDomain> cuda_pinned_;
  std::unique_ptr<IDomain> shared_;
  std::unique_ptr<IDomain> file_;
  mutable std::mutex ns_mtx_;
  std::unordered_map<NamespaceId, NamespaceConfig> nss_;
  std::unordered_map<NamespaceId, std::unordered_map<MemoryDomain, std::uint64_t>> ns_committed_;
  std::unordered_map<NamespaceId, std::unordered_map<MemoryDomain, std::uint64_t>> ns_reserved_;
  std::unordered_map<NamespaceId, std::unordered_map<MemoryDomain, std::uint64_t>> ns_allocated_;
  std::unordered_map<MemoryDomain, std::uint64_t> pool_idle_by_domain_;
  std::unordered_map<NamespaceId, std::uint64_t> ns_allocs_;
  std::atomic<std::uint64_t> id_counter_{0};
  std::uint64_t id_seed_ = 0;
  std::uint64_t epoch_ = 0;
  std::uint64_t peak_committed_ = 0;
  mutable std::mutex telemetry_mtx_;
  Telemetry telem_;
  std::deque<std::pair<std::string, std::string>> dec_;
  mutable std::mutex dec_mtx_;
  int device_cuda_ = -1;
};

}  // namespace internal
}  // namespace unified_buffer