#include "internal.hpp"
#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/export.hpp"
#include "unified_buffer/telemetry.hpp"
#include "unified_buffer/version.hpp"
#include <random>
#include <chrono>
#include <algorithm>
#include <cmath>

namespace unified_buffer {
namespace internal {

std::uint64_t now_epoch_ms() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

RuntimeImpl::RuntimeImpl(const RuntimeConfig& c) : cfg(c) {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  id_seed_ = gen();
  epoch_ = now_epoch_ms();
  id_counter_.store(1);

  if (cfg.enable_host) host_ = std::make_unique<HostDomain>(cfg.host_capacity_hint);

  int cuda_devs = 0;
#ifdef UNIFIED_BUFFER_HAS_CUDA
  if (cuda_support::available()) {
    cuda_devs = cuda_support::device_count();
    cuda_device_ = std::make_unique<CudaDeviceDomain>();
    cuda_pinned_ = std::make_unique<CudaPinnedDomain>();
    if (cuda_devs > 0) device_cuda_ = 0;
  }
#endif

  attach_shared_and_file_backends();

  NamespaceConfig def;
  def.name = "default";
  nss_[kDefaultNamespace] = def;
  for (const auto& nscfg : cfg.namespaces) {
    NamespaceId id = nss_.size() + 1;
    auto nsc = nscfg;
    if (nsc.name.empty()) nsc.name = "ns-" + std::to_string(id);
    nss_[id] = nsc;
  }
}

RuntimeImpl::~RuntimeImpl() { shutdown(); }

void RuntimeImpl::shutdown() {
  if (closed_.exchange(true)) return;
  std::vector<std::shared_ptr<Control>> todo;
  {
    std::unique_lock<std::shared_mutex> lk(registry_mtx_);
    for (auto& [id, ctl] : registry_) todo.push_back(ctl);
  }
  // Deterministic teardown: buffers with no outstanding references are
  // finalized now; any still-referenced buffer is cleaned up when its last
  // handle/lease drops (the handle holds a shared_ptr to this impl, so it stays
  // alive even after the Runtime object is destroyed).
  for (auto& ctl : todo) if (ctl->refs.load() == 0) finalize(ctl.get(), true);
  pool_trim_to_zero();
  cuda_device_.reset();
  cuda_pinned_.reset();
  shared_.reset();
  file_.reset();
  host_.reset();
}

IDomain* RuntimeImpl::domain(MemoryDomain d) const {
  switch (d) {
    case MemoryDomain::HOST: return host_.get();
    case MemoryDomain::PINNED_HOST: return cuda_pinned_.get();
    case MemoryDomain::DEVICE: return cuda_device_.get();
    case MemoryDomain::SHARED_HOST: return shared_.get();
    case MemoryDomain::MMAP_STORAGE: return file_.get();
  }
  return nullptr;
}

bool RuntimeImpl::domain_enabled(MemoryDomain d) const { return domain(d) != nullptr; }

const Capabilities& RuntimeImpl::domain_caps(MemoryDomain d) const {
  static const Capabilities kEmpty;
  auto* dom = domain(d);
  return dom ? dom->capabilities() : kEmpty;
}

Result<std::uint64_t> RuntimeImpl::capacity_for(MemoryDomain d) const {
  auto* dom = domain(d);
  if (!dom) return Error(ErrorCode::unsupported_domain, std::string("capability query: ") + std::string(to_string(d)) + " disabled");
  auto cap = dom->capacity();
  if (!cap.ok()) return Error(cap.error());
  std::uint64_t total = cap.value().total_capacity;
  std::uint64_t configured = config_cap(d);
  if (configured) return ok(std::min(configured, total ? total : configured));
  return ok(total);
}

std::uint64_t RuntimeImpl::cap_for(MemoryDomain d) const {
  auto cap = capacity_for(d);
  return cap.ok() ? cap.value() : 0;
}

std::uint64_t RuntimeImpl::config_cap(MemoryDomain d) const {
  switch (d) {
    case MemoryDomain::HOST: return cfg.host_cap;
    case MemoryDomain::PINNED_HOST: return cfg.pinned_cap;
    case MemoryDomain::DEVICE: return cfg.device_cap;
    case MemoryDomain::SHARED_HOST: return cfg.shared_cap;
    case MemoryDomain::MMAP_STORAGE: return cfg.file_cap;
  }
  return 0;
}

BufferId RuntimeImpl::next_id() {
  BufferId id;
  id.hi = id_seed_ ^ (epoch_ & 0xFFFFFFFFULL);
  id.lo = id_counter_.fetch_add(1, std::memory_order_relaxed);
  return id;
}

Status RuntimeImpl::create_namespace(const NamespaceConfig& cfg_in) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  NamespaceId id = nss_.size() + 1;
  NamespaceConfig c = cfg_in;
  if (c.name.empty()) c.name = "ns-" + std::to_string(id);
  nss_[id] = c;
  return ok_status();
}

std::optional<NamespaceConfig> RuntimeImpl::namespace_info(NamespaceId id) const {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto it = nss_.find(id);
  if (it == nss_.end()) return std::nullopt;
  return it->second;
}

// Caller must hold ns_mtx_.
std::uint64_t RuntimeImpl::domain_quota(NamespaceId ns, MemoryDomain d) const {
  auto it = nss_.find(ns);
  if (it == nss_.end()) return 0;
  switch (d) {
    case MemoryDomain::HOST: return it->second.host_quota;
    case MemoryDomain::PINNED_HOST: return it->second.pinned_quota;
    case MemoryDomain::DEVICE: return it->second.device_quota;
    case MemoryDomain::SHARED_HOST: return it->second.shared_quota;
    case MemoryDomain::MMAP_STORAGE: return it->second.file_quota;
  }
  return 0;
}

Status RuntimeImpl::reserve_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes) {
  if (bytes == 0) return ok_status();
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto& comm = ns_committed_[ns][d];
  auto& res = ns_reserved_[ns][d];
  std::uint64_t cap = cap_for(d);
  if (cap && (comm + res + bytes > cap)) return Error(ErrorCode::out_of_capacity, "reserve: domain capacity exceeded");
  std::uint64_t quota = domain_quota(ns, d);
  if (quota && (comm + res + bytes > quota)) { ++telem_.quota_rejections; return Error(ErrorCode::quota_exceeded, "reserve: quota exceeded"); }
  res += bytes;
  return ok_status();
}

Status RuntimeImpl::commit_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto& res = ns_reserved_[ns][d];
  auto& comm = ns_committed_[ns][d];
  auto& alloc = ns_allocated_[ns][d];
  if (res < bytes) return Error(ErrorCode::state_invalid, "commit: reservation mismatch");
  res -= bytes;
  comm += bytes;
  alloc += bytes;
  update_peak(comm + alloc);
  return ok_status();
}

void RuntimeImpl::unreserve_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto& res = ns_reserved_[ns][d];
  if (res >= bytes) res -= bytes; else res = 0;
}

void RuntimeImpl::checkout_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  ns_allocated_[ns][d] += bytes;
  auto& idle = pool_idle_by_domain_[d];
  idle = (idle >= bytes) ? idle - bytes : 0;
}

void RuntimeImpl::release_amount(NamespaceId ns, MemoryDomain d, std::uint64_t bytes, bool pooled) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto& alloc = ns_allocated_[ns][d];
  auto& comm = ns_committed_[ns][d];
  alloc = (alloc >= bytes) ? alloc - bytes : 0;
  if (pooled) {
    pool_idle_by_domain_[d] += bytes;
  } else {
    comm = (comm >= bytes) ? comm - bytes : 0;
  }
}

void RuntimeImpl::free_accounting(NamespaceId ns, MemoryDomain d, std::uint64_t bytes) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto& alloc = ns_allocated_[ns][d];
  auto& comm = ns_committed_[ns][d];
  alloc = (alloc >= bytes) ? alloc - bytes : 0;
  comm = (comm >= bytes) ? comm - bytes : 0;
}

Status RuntimeImpl::reserve_count(NamespaceId ns) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto& c = ns_allocs_[ns];
  std::uint64_t quota = 0;
  auto it = nss_.find(ns);
  if (it != nss_.end()) quota = it->second.allocation_count_quota;
  if (quota && c >= quota) { ++telem_.quota_rejections; return Error(ErrorCode::quota_exceeded, "allocation count quota exceeded"); }
  ++c;
  return ok_status();
}

void RuntimeImpl::release_count(NamespaceId ns) {
  std::lock_guard<std::mutex> lk(ns_mtx_);
  auto& c = ns_allocs_[ns];
  if (c > 0) --c;
}

void RuntimeImpl::update_peak(std::uint64_t committed) {
  if (committed > peak_committed_) peak_committed_ = committed;
}


// ---------------------------------------------------------------------------
// Validation, size classes, pool keys
// ---------------------------------------------------------------------------
std::uint64_t RuntimeImpl::size_class_for(std::uint64_t bytes) const {
  const std::uint64_t min = cfg.pool.min_class_size ? cfg.pool.min_class_size : 4096;
  const std::uint64_t max = cfg.pool.max_class_size ? cfg.pool.max_class_size : (256ULL * 1024 * 1024);
  std::uint64_t cls = min;
  while (cls < bytes && cls < max) cls <<= 1;
  return cls;
}

PoolKey RuntimeImpl::make_pool_key(const Record& rec) const {
  PoolKey key;
  key.domain = rec.domain;
  key.backend = rec.backend;
  key.device = rec.device;
  key.size_class = size_class_for(rec.size);
  key.alignment = rec.alignment ? rec.alignment : 1;
  key.exportable = rec.flags.exportable;
  return key;
}

Status RuntimeImpl::validate_request(const AllocationRequest& req, std::uint64_t& alignment, std::uint64_t& class_size) {
  if (req.size == 0) return Error(ErrorCode::invalid_argument, "allocation: zero-size request");
  if (req.size > (1ULL << 48)) return Error(ErrorCode::invalid_argument, "allocation: impossible size");
  auto* dom = domain(req.domain);
  if (!dom) return Error(ErrorCode::unsupported_domain, std::string("allocation: domain ") + std::string(to_string(req.domain)) + " not enabled");
  alignment = req.alignment;
  if (alignment == 0) {
    auto pa = dom->preferred_alignment();
    alignment = pa.ok() ? pa.value() : 0;
  }
  if (alignment == 0) alignment = 64;
  if ((alignment & (alignment - 1)) != 0) return Error(ErrorCode::alignment_error, "allocation: alignment not power-of-two");
  if (alignment > 4096) return Error(ErrorCode::alignment_error, "allocation: alignment too large");
  if (req.domain == MemoryDomain::DEVICE) {
    if (req.device.index < 0) return Error(ErrorCode::invalid_device, "allocation: DEVICE requires a device index");
  }
  auto up = align_up(req.size, alignment);
  if (!up) return Error(ErrorCode::overflow, "allocation: size+alignment overflow");
  class_size = up.value();
  return ok_status();
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------
Result<Record> RuntimeImpl::allocate_record(const AllocationRequest& req) {
  if (closed()) return Error(ErrorCode::closed, "runtime is closed");
  std::uint64_t alignment = 0;
  std::uint64_t class_size = 0;
  auto v = validate_request(req, alignment, class_size);
  if (!v.ok()) { ++telem_.failed_allocations; return Error(v.error()); }

  auto quota = reserve_count(req.ns);
  if (!quota.ok()) { ++telem_.failed_allocations; return Error(quota.error()); }

  auto res = reserve_amount(req.ns, req.domain, class_size);
  if (!res.ok()) { release_count(req.ns); ++telem_.failed_allocations; ++telem_.reservation_failures; return Error(res.error()); }

  Record rec;
  rec.id = next_id();
  rec.generation = 1;
  rec.ns = req.ns;
  rec.domain = req.domain;
  rec.backend = domain(req.domain)->backend();
  rec.device = (req.domain == MemoryDomain::DEVICE) ? req.device : DeviceId{rec.backend, -1};
  rec.size = req.size;
  rec.alignment = alignment;
  rec.flags = req.flags;
  rec.access = req.access;
  rec.label = req.label;
  rec.created_epoch = now_epoch_ms();
  rec.ownership = Ownership::RUNTIME;
  rec.meta_crc = 0;

  // Pool attempt.
  bool from_pool = false;
  std::optional<NamespaceId> last_owner;
  if (req.flags.pooled && cfg.enable_pool && cfg.pool.enabled) {
    PoolKey key = make_pool_key(rec);
    auto slot = pool_take(key, req.ns, class_size);
    if (slot) {
      rec.backing = std::move(slot->backing);
      rec.pooled = true;
      rec.committed = rec.backing.committed;
      from_pool = true;
      last_owner = slot->last_owner;
      // Pooling the same class may be from a different namespace; the slot
      // records the previous owner for zeroing policy.
      checkout_amount(req.ns, req.domain, rec.committed);
    } else {
      ++telem_.pool_misses;
    }
  }

  if (!from_pool) {
      auto alloc = domain(req.domain)->allocate(class_size, alignment);
      if (!alloc.ok()) {
      unreserve_amount(req.ns, req.domain, class_size);
      release_count(req.ns);
      ++telem_.failed_allocations;
      return Error(alloc.error());
    }
    rec.backing = std::move(alloc.value());
    rec.committed = rec.backing.committed;
    rec.pooled = false;
    auto commit = commit_amount(req.ns, req.domain, rec.committed);
    if (!commit.ok()) {
      domain(req.domain)->free(rec.backing);
      unreserve_amount(req.ns, req.domain, class_size);
      release_count(req.ns);
      ++telem_.failed_allocations;
      return Error(commit.error());
    }
  }

  // Pool eligibility applies to both fresh and pooled allocations.
  rec.pool_eligible = cfg.enable_pool && cfg.pool.enabled && rec.size >= cfg.pool.min_class_size && size_class_for(rec.size) <= cfg.pool.max_class_size;
  if (from_pool) rec.pooled = true;

  // Zeroing policy.
  bool want_zero = false;
  ZeroingPolicy zp = ZeroingPolicy::ON_CROSS_OWNER_REUSE;
  if (req.zeroing) {
    zp = *req.zeroing;
  } else {
    auto ns = namespace_info(req.ns);
    if (ns) zp = ns->zeroing;
  }
  switch (zp) {
    case ZeroingPolicy::NEVER: want_zero = false; break;
    case ZeroingPolicy::ON_ALLOCATE: want_zero = true; break;
    case ZeroingPolicy::ON_RELEASE: want_zero = false; break;
    case ZeroingPolicy::ON_CROSS_OWNER_REUSE: want_zero = from_pool && last_owner && (*last_owner != req.ns); break;
    case ZeroingPolicy::ALWAYS: want_zero = true; break;
  }
  if (want_zero) {
    auto z = domain(req.domain)->zero(rec.backing, 0, rec.backing.committed);
    if (!z.ok()) {
      // On zero failure (rare), roll back cleanly.
      if (from_pool) { free_accounting(req.ns, req.domain, rec.committed); } else { unreserve_amount(req.ns, req.domain, class_size); domain(req.domain)->free(rec.backing); release_count(req.ns); }
      ++telem_.failed_allocations;
      return Error(z.error());
    }
    ++telem_.zero_count;
  }

  rec.state = BufferState::ALLOCATED;
  return ok(std::move(rec));
}

std::shared_ptr<Control> RuntimeImpl::register_control(Record rec) {
  auto ctl = std::make_shared<Control>();
  ctl->rt = shared();
  ctl->rec = std::move(rec);
  ctl->pool_eligible = ctl->rec.pool_eligible;
  {
    std::unique_lock<std::shared_mutex> lk(registry_mtx_);
    registry_[ctl->rec.id] = ctl;
  }
  ++telem_.total_allocations;
  ++telem_.active_buffers;
  ++telem_.outstanding_allocations;
  telem_.bytes_requested += ctl->rec.size;
  return ctl;
}

void RuntimeImpl::retain(Control* c) { c->refs.fetch_add(1, std::memory_order_relaxed); }

void RuntimeImpl::drop_ref(Control* c) {
  int prev = c->refs.fetch_sub(1, std::memory_order_acq_rel);
  if (prev == 1) finalize(c, true);
}

// ---------------------------------------------------------------------------
// Finalization
// ---------------------------------------------------------------------------
void RuntimeImpl::finalize(Control* ctl, bool return_to_pool) {
  std::lock_guard<std::mutex> lk(ctl->mtx);
  if (ctl->finalized.exchange(true)) return;
  Record& rec = ctl->rec;
  bool backing_present = rec.backing.pointer != nullptr || rec.backing.host_map != nullptr;
  bool went_pooled = false;

  if (backing_present && rec.ownership != Ownership::BORROWED) {
    if (return_to_pool && rec.pool_eligible && rec.flags.pooled && cfg.enable_pool && cfg.pool.enabled) {
      // Secure reuse: if the pool will hand this backing to another owner,
      // apply the policy zeroing before it returns to the pool.
      auto ns = namespace_info(rec.ns);
      if (ns && (ns->zeroing == ZeroingPolicy::ON_RELEASE || ns->zeroing == ZeroingPolicy::ALWAYS)) {
        auto* zdom = domain(rec.domain);
        if (zdom) { zdom->zero(rec.backing, 0, rec.committed); ++telem_.zero_count; }
      }
      PoolKey key = make_pool_key(rec);
      PoolSlot slot;
      slot.backing = std::move(rec.backing);
      slot.original_id = rec.id;
      slot.last_owner = rec.ns;
      went_pooled = pool_put(key, std::move(slot));
      if (went_pooled) {
        release_amount(rec.ns, rec.domain, rec.committed, true);
      } else {
        free_accounting(rec.ns, rec.domain, rec.committed);
      }
    } else {
      if (rec.ownership == Ownership::ADOPTED) rec.ownership = Ownership::RUNTIME;
      auto* dom = domain(rec.domain);
      if (dom) dom->free(rec.backing);
      free_accounting(rec.ns, rec.domain, rec.committed);
      rec.backing = NativeAllocation{};
    }
  } else {
    rec.backing = NativeAllocation{};
  }

  rec.state = BufferState::RELEASED;
  release_count(rec.ns);

  {
    std::unique_lock<std::shared_mutex> ulk(registry_mtx_);
    registry_.erase(rec.id);
  }
  ++telem_.total_frees;
  if (telem_.active_buffers > 0) --telem_.active_buffers;
  if (telem_.outstanding_allocations > 0) --telem_.outstanding_allocations;
  note("finalize", std::string(to_string(rec.domain)) + " id=" + std::to_string(rec.id.lo) + (went_pooled ? " ->pool" : " ->free"));
}

Status RuntimeImpl::finalize_by_id(BufferId id) {
  std::shared_ptr<Control> ctl;
  {
    std::unique_lock<std::shared_mutex> lk(registry_mtx_);
    auto it = registry_.find(id);
    if (it == registry_.end()) return Error(ErrorCode::stale_handle, "finalize_by_id: not found");
    ctl = it->second;
  }
  std::lock_guard<std::mutex> lk(ctl->mtx);
  if (ctl->refs.load() > 0) return Error(ErrorCode::resource_busy, "finalize_by_id: buffer still has references");
  if (ctl->finalized.load()) return ok_status();
  ctl->refs.store(0);
  finalize(ctl.get(), true);
  return ok_status();
}

Result<std::shared_ptr<Control>> RuntimeImpl::lookup(BufferId id, BufferGeneration expect_gen) {
  std::shared_lock<std::shared_mutex> lk(registry_mtx_);
  auto it = registry_.find(id);
  if (it == registry_.end()) { ++telem_.stale_handle_rejections; return Error(ErrorCode::stale_handle, "lookup: unknown buffer id"); }
  auto ctl = it->second;
  if (expect_gen != ctl->rec.generation) { ++telem_.stale_generation_rejections; return Error(ErrorCode::stale_generation, "lookup: generation mismatch"); }
  return ok(ctl);
}

// ---------------------------------------------------------------------------
// Pooling
// ---------------------------------------------------------------------------
std::optional<PoolSlot> RuntimeImpl::pool_take(const PoolKey& key, NamespaceId, std::uint64_t min_committed) {
  std::lock_guard<std::mutex> lk(pool_mtx_);
  auto it = pool_.find(key);
  if (it == pool_.end()) return std::nullopt;
  auto& dq = it->second;
  // Only reuse a slot whose backing is large enough for the request.  Because
  // the pool bucket is a power-of-two class while the real allocation is the
  // align_up(size, alignment), slots in the same bucket can differ in size.
  for (auto it2 = dq.begin(); it2 != dq.end(); ++it2) {
    if (it2->backing.committed >= min_committed) {
      PoolSlot slot = std::move(*it2);
      dq.erase(it2);
      if (pool_object_count_ > 0) --pool_object_count_;
      if (pool_idle_bytes_ >= slot.backing.committed) pool_idle_bytes_ -= slot.backing.committed; else pool_idle_bytes_ = 0;
      ++telem_.pool_hits;
      return slot;
    }
  }
  return std::nullopt;
}

bool RuntimeImpl::pool_put(PoolKey key, PoolSlot slot) {
  std::lock_guard<std::mutex> lk(pool_mtx_);
  const std::uint64_t sz = slot.backing.committed;
  if (pool_object_count_ >= cfg.pool.max_objects || pool_idle_bytes_ + sz > cfg.pool.max_idle_bytes) {
    // Evict: free the backing directly.
    ++telem_.pool_evictions;
    auto* dom = domain(key.domain);
    if (dom) dom->free(slot.backing);
    return false;
  }
  pool_[key].push_back(std::move(slot));
  ++pool_object_count_;
  pool_idle_bytes_ += sz;
  return true;
}

Result<std::size_t> RuntimeImpl::pool_size() const {
  std::lock_guard<std::mutex> lk(pool_mtx_);
  std::size_t n = 0;
  for (auto& [k, dq] : pool_) n += dq.size();
  return ok(n);
}

Result<std::uint64_t> RuntimeImpl::pool_bytes() const {
  std::lock_guard<std::mutex> lk(pool_mtx_);
  return ok(pool_idle_bytes_);
}

Status RuntimeImpl::pool_trim() {
  std::lock_guard<std::mutex> lk(pool_mtx_);
  for (auto& [key, dq] : pool_) {
    while (!dq.empty()) {
      PoolSlot& slot = dq.front();
      // Freeing the backing resets NativeAllocation::committed to 0, so capture
      // the committed size before the backend frees it.
      const std::uint64_t committed = slot.backing.committed;
      auto* dom = domain(key.domain);
      if (dom) dom->free(slot.backing);
      free_accounting(slot.last_owner, key.domain, committed);
      if (pool_object_count_ > 0) --pool_object_count_;
      if (pool_idle_bytes_ >= committed) pool_idle_bytes_ -= committed; else pool_idle_bytes_ = 0;
      dq.pop_front();
    }
  }
  return ok_status();
}

void RuntimeImpl::pool_trim_to_zero() { pool_trim(); }

Result<std::shared_ptr<Control>> RuntimeImpl::wrap_external(const ExternalMemoryDesc& desc) {
  if (closed()) return Error(ErrorCode::closed, "runtime is closed");
  if (!desc.pointer || desc.size == 0) return Error(ErrorCode::invalid_argument, "wrap_external: null pointer or zero size");
  auto* dom = domain(desc.domain);
  if (!dom) return Error(ErrorCode::unsupported_domain, "wrap_external: domain not enabled");
  auto res = reserve_count(kDefaultNamespace);
  if (!res.ok()) return Error(res.error());
  Record rec;
  rec.id = next_id();
  rec.generation = 1;
  rec.ns = kDefaultNamespace;
  rec.domain = desc.domain;
  rec.backend = dom->backend();
  rec.device = desc.device;
  rec.size = desc.size;
  rec.committed = desc.size;
  rec.alignment = desc.alignment ? desc.alignment : 64;
  rec.access = (desc.ownership == ExternalOwnership::BORROW) ? AccessMode::READ : AccessMode::READ_WRITE;
  rec.ownership = (desc.ownership == ExternalOwnership::BORROW) ? Ownership::BORROWED
                 : (desc.ownership == ExternalOwnership::ADOPT) ? Ownership::ADOPTED : Ownership::SHARED;
  rec.flags.zero_on_alloc = false;
  rec.flags.pooled = false;
  rec.state = BufferState::ALLOCATED;
  rec.backing.pointer = desc.pointer;
  rec.backing.host_map = desc.pointer;
  rec.backing.size = desc.size;
  rec.backing.committed = desc.size;
  rec.backing.alignment = rec.alignment;
  rec.backing.domain = desc.domain;
  rec.backing.backend = rec.backend;
  rec.backing.device = desc.device;
  auto ctl = register_control(std::move(rec));
  return ok(ctl);
}


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool RuntimeImpl::valid_transition(BufferState from, BufferState to) {
  using S = BufferState;
  switch (from) {
    case S::DECLARED: return to == S::RESERVED || to == S::ALLOCATED || to == S::FAILED || to == S::INVALID;
    case S::RESERVED: return to == S::ALLOCATED || to == S::RELEASED || to == S::FAILED || to == S::INVALID;
    case S::ALLOCATED: return to == S::MAPPED || to == S::COPYING || to == S::IN_USE || to == S::EXPORTING || to == S::RELEASING || to == S::MIGRATING || to == S::RELEASED || to == S::FAILED || to == S::INVALID;
    case S::MAPPED: return to == S::ALLOCATED || to == S::IN_USE || to == S::COPYING || to == S::RELEASING || to == S::RELEASED || to == S::FAILED || to == S::INVALID;
    case S::IN_USE: return to == S::ALLOCATED || to == S::COPYING || to == S::RELEASING || to == S::RELEASED || to == S::FAILED || to == S::INVALID;
    case S::EXPORTING: return to == S::EXPORTED || to == S::ALLOCATED || to == S::FAILED;
    case S::EXPORTED: return to == S::ALLOCATED || to == S::RELEASED || to == S::FAILED;
    case S::IMPORTING: return to == S::IMPORTED || to == S::FAILED || to == S::INVALID;
    case S::IMPORTED: return to == S::ALLOCATED || to == S::RELEASED || to == S::FAILED || to == S::INVALID;
    case S::COPYING: return to == S::ALLOCATED || to == S::IN_USE || to == S::RELEASED || to == S::FAILED;
    case S::MIGRATING: return to == S::ALLOCATED || to == S::FAILED;
    case S::RELEASING: return to == S::RELEASED || to == S::FAILED;
    case S::RELEASED: return to == S::INVALID;
    case S::FAILED: return to == S::QUARANTINED || to == S::INVALID;
    case S::QUARANTINED: return to == S::INVALID;
    case S::INVALID: return false;
  }
  return false;
}

Status RuntimeImpl::set_state(Control* ctl, BufferState to) {
  std::lock_guard<std::mutex> lk(ctl->mtx);
  if (!valid_transition(ctl->rec.state, to)) {
    return Error(ErrorCode::lifecycle_violation, std::string("illegal transition ") + std::string(to_string(ctl->rec.state)) + " -> " + std::string(to_string(to)));
  }
  ctl->rec.state = to;
  return ok_status();
}

Status RuntimeImpl::transition(Control* ctl, BufferState to) { return set_state(ctl, to); }

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------
RuntimeImpl::Telemetry RuntimeImpl::telemetry() const {
  Telemetry t = telem_;
  {
    std::shared_lock<std::shared_mutex> lk(registry_mtx_);
    t.active_buffers = registry_.size();
    t.outstanding_allocations = registry_.size();
  }
  {
    std::lock_guard<std::mutex> lk(ns_mtx_);
    for (auto& [ns, bydom] : ns_committed_) {
      (void)ns;
      for (auto& [d, bytes] : bydom) {
        switch (d) {
          case MemoryDomain::HOST: t.host_bytes += bytes; break;
          case MemoryDomain::PINNED_HOST: t.pinned_host_bytes += bytes; break;
          case MemoryDomain::DEVICE: t.device_bytes += bytes; break;
          case MemoryDomain::SHARED_HOST: t.shared_host_bytes += bytes; break;
          case MemoryDomain::MMAP_STORAGE: t.file_backed_bytes += bytes; break;
        }
      }
    }
  }
  t.peak_bytes = peak_committed_;
  t.bytes_committed = t.host_bytes + t.pinned_host_bytes + t.device_bytes + t.shared_host_bytes + t.file_backed_bytes;
  {
    std::lock_guard<std::mutex> lk(pool_mtx_);
    t.pooled_bytes = pool_idle_bytes_;
    t.idle_pooled_bytes = pool_idle_bytes_;
  }
  return t;
}

void RuntimeImpl::note(const char* event, const std::string& detail) {
  std::lock_guard<std::mutex> lk(dec_mtx_);
  if (dec_.size() > 4096) dec_.pop_front();
  dec_.emplace_back(std::string(event), detail);
}

std::vector<std::pair<std::string, std::string>> RuntimeImpl::decisions() const {
  std::lock_guard<std::mutex> lk(dec_mtx_);
  return std::vector<std::pair<std::string, std::string>>(dec_.begin(), dec_.end());
}

Result<std::shared_ptr<Control>> RuntimeImpl::migrate(const std::shared_ptr<Control>& src, MemoryDomain target) {
  if (closed()) return Error(ErrorCode::closed, "runtime is closed");
  std::lock_guard<std::mutex> lk(src->mtx);
  Record& rec = src->rec;
  if (rec.domain == target) return ok(src);
  if (rec.state == BufferState::RELEASED || rec.state == BufferState::INVALID) return Error(ErrorCode::lifecycle_violation, "migrate: buffer released");
  if (src->write_leases > 0 || src->exclusive_leases > 0) { ++telem_.lease_conflicts; return Error(ErrorCode::lease_conflict, "migrate: active write/exclusive lease"); }
  auto* tdom = domain(target);
  if (!tdom) return Error(ErrorCode::unsupported_domain, std::string("migrate: target domain ") + std::string(to_string(target)) + " disabled");
  std::uint64_t class_size = rec.committed ? rec.committed : rec.size;
  DeviceId tdev;
  if (target == MemoryDomain::DEVICE) {
    tdev = DeviceId{BackendId::CUDA, device_cuda_ >= 0 ? device_cuda_ : 0};
  } else {
    tdev = DeviceId{tdom->backend(), -1};
  }

  auto res = reserve_amount(rec.ns, target, class_size);
  if (!res.ok()) return Error(res.error());
  auto alloc = tdom->allocate(class_size, rec.alignment);
  if (!alloc.ok()) { unreserve_amount(rec.ns, target, class_size); return Error(alloc.error()); }
  auto newbacking = std::move(alloc.value());
  const std::uint64_t len = std::min(rec.committed, newbacking.committed);
  const bool dst_dev = target == MemoryDomain::DEVICE;
  const bool src_dev = rec.domain == MemoryDomain::DEVICE;
  void* dp = newbacking.host_map ? newbacking.host_map : newbacking.pointer;
  const void* sp = rec.backing.host_map ? rec.backing.host_map : rec.backing.pointer;
  auto cp = copy_memory(dp, sp, len, dst_dev, src_dev);
  if (!cp.ok()) { tdom->free(newbacking); unreserve_amount(rec.ns, target, class_size); return Error(cp.error()); }
  auto commit = commit_amount(rec.ns, target, newbacking.committed);
  if (!commit.ok()) { tdom->free(newbacking); unreserve_amount(rec.ns, target, class_size); return Error(commit.error()); }
  // Verify destination before discarding the source.
  auto vchk = tdom->verify(newbacking, 0, newbacking.committed);
  if (!vchk.ok()) { tdom->free(newbacking); if (target != MemoryDomain::DEVICE) free_accounting(rec.ns, target, newbacking.committed); unreserve_amount(rec.ns, target, class_size); return Error(vchk.error()); }
  // Swap binding.
  MemoryDomain old_domain = rec.domain;
  auto oldbacking = std::move(rec.backing);
  std::uint64_t old_committed = rec.committed;
  rec.backing = std::move(newbacking);
  rec.domain = target;
  rec.backend = tdom->backend();
  rec.device = tdev;
  rec.committed = rec.backing.committed;
  rec.pooled = false;
  rec.pool_eligible = false;
  ++rec.generation;
  rec.state = BufferState::ALLOCATED;
  rec.meta_crc = meta_crc_of(rec);
  // Release the old authority.
  if (oldbacking.pointer || oldbacking.host_map) {
    auto* odom = domain(old_domain);
    if (odom) odom->free(oldbacking);
    free_accounting(rec.ns, old_domain, old_committed);
  }
  ++telem_.migration_count;
  note("migrate", std::string(to_string(old_domain)) + " -> " + std::string(to_string(target)) + " id=" + std::to_string(rec.id.lo));
  return ok(src);
}

}  // namespace internal


// ---------------------------------------------------------------------------
// Public Runtime facade
// ---------------------------------------------------------------------------
Runtime::Runtime(const RuntimeConfig& cfg) : impl_(std::make_shared<internal::RuntimeImpl>(cfg)) {}
Runtime::~Runtime() { if (impl_) impl_->shutdown(); }
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

Result<BufferHandle> Runtime::allocate(const AllocationRequest& req) {
  if (!impl_ || impl_->closed()) return Error(ErrorCode::closed, "runtime is closed");
  auto rec = impl_->allocate_record(req);
  if (!rec.ok()) return Error(rec.error());
  auto ctl = impl_->register_control(rec.value());
  impl_->retain(ctl.get());
  BufferHandle h;
  h.rt_ = impl_;
  h.ctl_ = ctl;
  h.id_ = ctl->rec.id;
  h.gen_ = ctl->rec.generation;
  h.valid_ = true;
  return ok(std::move(h));
}

Result<BufferHandle> Runtime::wrap_external(const ExternalMemoryDesc& desc) {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  auto ctl = impl_->wrap_external(desc);
  if (!ctl.ok()) return Error(ctl.error());
  impl_->retain(ctl.value().get());
  BufferHandle h;
  h.rt_ = impl_; h.ctl_ = ctl.value(); h.id_ = ctl.value()->rec.id; h.gen_ = ctl.value()->rec.generation; h.valid_ = true;
  return ok(std::move(h));
}

Result<BufferHandle> Runtime::migrate(const BufferHandle& h, MemoryDomain target) {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  auto alive = h.validate_alive();
  if (!alive.ok()) return Error(alive.error());
  auto ctl = impl_->migrate(h.ctl_, target);
  if (!ctl.ok()) return Error(ctl.error());
  // The migrated control has a new generation; return a fresh handle.
  BufferHandle out;
  out.rt_ = impl_; out.ctl_ = ctl.value(); out.id_ = ctl.value()->rec.id; out.gen_ = ctl.value()->rec.generation; out.valid_ = true;
  impl_->retain(ctl.value().get());
  return ok(std::move(out));
}

Result<ExportDescriptor> Runtime::export_buffer(const BufferHandle& h) {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  auto alive = h.validate_alive();
  if (!alive.ok()) return Error(alive.error());
  return impl_->export_descriptor(*h.ctl_);
}

Result<BufferHandle> Runtime::import(const ExportDescriptor& desc, NamespaceId ns, const std::string& label) {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  auto ctl = impl_->import_descriptor(desc, ns, label);
  if (!ctl.ok()) return Error(ctl.error());
  impl_->retain(ctl.value().get());
  BufferHandle h;
  h.rt_ = impl_; h.ctl_ = ctl.value(); h.id_ = ctl.value()->rec.id; h.gen_ = ctl.value()->rec.generation; h.valid_ = true;
  return ok(std::move(h));
}

Status Runtime::invalidate(BufferId id) {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return impl_->finalize_by_id(id);
}
Status Runtime::pool_trim() {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return impl_->pool_trim();
}
void Runtime::shutdown() { if (impl_) impl_->shutdown(); }
bool Runtime::closed() const { return impl_ ? impl_->closed() : true; }

Status Runtime::create_namespace(const NamespaceConfig& cfg) {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return impl_->create_namespace(cfg);
}
Result<NamespaceConfig> Runtime::namespace_info(NamespaceId id) const {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  auto info = impl_->namespace_info(id);
  if (!info) return Error(ErrorCode::not_found, "namespace not found");
  return ok(*info);
}
Result<RuntimeStats> Runtime::stats() const {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return impl_->runtime_stats();
}
Result<std::vector<BackendInfo>> Runtime::backends() const {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return impl_->backends_info();
}
Result<std::vector<DeviceInfo>> Runtime::devices() const {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return impl_->devices_info();
}
Result<std::vector<DomainInfo>> Runtime::domains() const {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return impl_->domains_info();
}
Result<std::vector<std::pair<std::string, std::string>>> Runtime::decisions() const {
  if (!impl_) return Error(ErrorCode::closed, "runtime is closed");
  return ok(impl_->decisions());
}
}  // namespace unified_buffer