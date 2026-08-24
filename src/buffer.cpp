#include "internal.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/runtime.hpp"
#include <cstring>
#ifdef UNIFIED_BUFFER_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace unified_buffer {


// ---------------------------------------------------------------------------
// BufferHandle
// ---------------------------------------------------------------------------
BufferHandle::BufferHandle(BufferHandle&& o) noexcept
  : rt_(std::move(o.rt_)), ctl_(std::move(o.ctl_)), id_(o.id_), gen_(o.gen_), valid_(o.valid_) {
  o.valid_ = false; o.ctl_.reset();
}
BufferHandle& BufferHandle::operator=(BufferHandle&& o) noexcept {
  if (this != &o) {
    reset();
    rt_ = std::move(o.rt_); ctl_ = std::move(o.ctl_); id_ = o.id_; gen_ = o.gen_; valid_ = o.valid_;
    o.valid_ = false; o.ctl_.reset();
  }
  return *this;
}
BufferHandle::~BufferHandle() { reset(); }

void BufferHandle::reset() {
  if (valid_ && ctl_) {
    auto rt = rt_;
    auto ctl = ctl_;
    if (rt) rt->drop_ref(ctl.get());
  }
  valid_ = false;
  ctl_.reset();
  id_ = BufferId{};
  gen_ = 0;
}

Status BufferHandle::validate_alive() const {
  if (!valid_ || !ctl_) return Error(ErrorCode::stale_handle, "handle is not alive");
  if (ctl_->finalized.load(std::memory_order_acquire)) return Error(ErrorCode::stale_handle, "buffer has been finalized");
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  if (gen_ != ctl_->rec.generation) return Error(ErrorCode::stale_generation, "stale generation");
  auto s = ctl_->rec.state;
  if (s == BufferState::RELEASED || s == BufferState::QUARANTINED || s == BufferState::INVALID)
    return Error(ErrorCode::stale_handle, "buffer is no longer valid");
  return ok_status();
}

Status BufferHandle::ensure_valid(AccessMode want) const {
  auto v = validate_alive();
  if (!v.ok()) return v;
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  if (want == AccessMode::WRITE && ctl_->rec.access == AccessMode::READ)
    return Error(ErrorCode::permission_failure, "write denied: buffer is read-only");
  return ok_status();
}

MemoryDomain BufferHandle::domain() const { return ctl_ ? ctl_->rec.domain : MemoryDomain::HOST; }
std::uint64_t BufferHandle::size() const { return ctl_ ? ctl_->rec.size : 0; }
std::uint64_t BufferHandle::aligned_size() const { return ctl_ ? ctl_->rec.committed : 0; }
std::uint64_t BufferHandle::alignment() const { return ctl_ ? ctl_->rec.alignment : 0; }
NamespaceId BufferHandle::namespace_id() const { return ctl_ ? ctl_->rec.ns : kDefaultNamespace; }
BufferState BufferHandle::state() const {
  if (!ctl_) return BufferState::INVALID;
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  return ctl_->rec.state;
}

Status BufferHandle::release() {
  if (!valid_ || !ctl_) return ok_status();
  auto rt = rt_;
  auto ctl = ctl_;
  valid_ = false;
  ctl_.reset();
  if (rt) rt->drop_ref(ctl.get());
  return ok_status();
}

Result<BufferHandle> BufferHandle::share() const {
  auto v = validate_alive();
  if (!v.ok()) return Error(v.error());
  rt_->retain(ctl_.get());
  BufferHandle h;
  h.rt_ = rt_; h.ctl_ = ctl_; h.id_ = id_; h.gen_ = gen_; h.valid_ = true;
  return ok(std::move(h));
}

Result<BufferView> BufferHandle::view(std::uint64_t offset, std::uint64_t len, AccessMode access) {
  auto v = ensure_valid(access);
  if (!v.ok()) return Error(v.error());
  {
    std::lock_guard<std::mutex> lk(ctl_->mtx);
    if (!range_in_bounds(offset, len, ctl_->rec.size)) return Error(ErrorCode::bounds_error, "view: offset+length exceeds buffer");
    if (len == 0) return Error(ErrorCode::invalid_argument, "view: zero length");
    if (access == AccessMode::WRITE && ctl_->rec.access == AccessMode::READ) return Error(ErrorCode::permission_failure, "view: write of read-only buffer");
  }
  BufferView bv;
  void* data = nullptr;
  {
    std::lock_guard<std::mutex> lk(ctl_->mtx);
    if (ctl_->rec.backing.host_map) data = static_cast<char*>(ctl_->rec.backing.host_map) + offset;
  }
  bv.rt_ = rt_; bv.ctl_ = ctl_; bv.gen_ = gen_; bv.off_ = offset; bv.len_ = len; bv.access_ = access; bv.data_ = data;
  rt_->retain(ctl_.get());   // pin the parent so it cannot finalize under this view
  return ok(std::move(bv));
}

Result<BufferMap> BufferHandle::map(AccessMode access) {
  auto v = ensure_valid(access);
  if (!v.ok()) return Error(v.error());
  if (ctl_->rec.domain == MemoryDomain::DEVICE) return Error(ErrorCode::unsupported_capability, "map: device memory is not CPU-mappable");
  auto* dom = rt_->domain(ctl_->rec.domain);
  if (!dom) return Error(ErrorCode::unsupported_domain, "map: domain disabled");
  void* p = nullptr;
  {
    std::lock_guard<std::mutex> lk(ctl_->mtx);
    auto m = dom->map(ctl_->rec.backing, access);
    if (!m.ok()) return Error(m.error());
    p = m.value();
    ++ctl_->rec.map_count;
    if (rt_->valid_transition(ctl_->rec.state, BufferState::MAPPED)) ctl_->rec.state = BufferState::MAPPED;
  }
  BufferMap bm;
  bm.rt_ = rt_; bm.ctl_ = ctl_; bm.gen_ = gen_; bm.access_ = access; bm.data_ = p;
  rt_->retain(ctl_.get());
  ++rt_->counters().map_count;
  return ok(std::move(bm));
}

Result<BufferLease> BufferHandle::acquire_read() {
  auto v = validate_alive(); if (!v.ok()) return Error(v.error());
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  if (ctl_->exclusive_leases > 0 || ctl_->write_leases > 0) { ++rt_->counters().lease_conflicts; return Error(ErrorCode::lease_conflict, "read lease conflicts with write/exclusive"); }
  ++ctl_->read_leases;
  if (ctl_->rec.state == BufferState::ALLOCATED) ctl_->rec.state = BufferState::IN_USE;
  rt_->retain(ctl_.get());
  return ok(BufferLease(rt_, ctl_, gen_, LeaseKind::READ));
}
Result<BufferLease> BufferHandle::acquire_write() {
  auto v = validate_alive(); if (!v.ok()) return Error(v.error());
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  if (ctl_->exclusive_leases > 0 || ctl_->write_leases > 0 || ctl_->read_leases > 0) { ++rt_->counters().lease_conflicts; return Error(ErrorCode::lease_conflict, "write lease conflicts with active leases"); }
  ++ctl_->write_leases;
  if (ctl_->rec.state == BufferState::ALLOCATED || ctl_->rec.state == BufferState::MAPPED) ctl_->rec.state = BufferState::IN_USE;
  rt_->retain(ctl_.get());
  return ok(BufferLease(rt_, ctl_, gen_, LeaseKind::WRITE));
}
Result<BufferLease> BufferHandle::acquire_exclusive() {
  auto v = validate_alive(); if (!v.ok()) return Error(v.error());
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  if (ctl_->exclusive_leases > 0 || ctl_->write_leases > 0 || ctl_->read_leases > 0) { ++rt_->counters().lease_conflicts; return Error(ErrorCode::lease_conflict, "exclusive lease conflicts with active leases"); }
  ++ctl_->exclusive_leases;
  if (ctl_->rec.state == BufferState::ALLOCATED || ctl_->rec.state == BufferState::MAPPED) ctl_->rec.state = BufferState::IN_USE;
  rt_->retain(ctl_.get());
  return ok(BufferLease(rt_, ctl_, gen_, LeaseKind::EXCLUSIVE));
}

Status BufferHandle::verify() {
  auto v = validate_alive(); if (!v.ok()) return Error(v.error());
  ++rt_->counters().integrity_checks;
  auto* dom = rt_->domain(ctl_->rec.domain);
  if (!dom) return Error(ErrorCode::unsupported_domain, "verify: domain disabled");
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  if (ctl_->rec.state == BufferState::RELEASED) return Error(ErrorCode::stale_handle, "verify: buffer released");
  auto chk = dom->verify(ctl_->rec.backing, 0, ctl_->rec.committed);
  if (!chk.ok()) { ++rt_->counters().integrity_failures; return Error(chk.error()); }
  // Metadata integrity.
  auto mc = meta_crc_of(ctl_->rec);
  if (ctl_->rec.meta_crc != 0 && mc != ctl_->rec.meta_crc) { ++rt_->counters().integrity_failures; return Error(ErrorCode::integrity_failure, "verify: metadata checksum mismatch"); }
  return ok_status();
}

Result<std::uint32_t> BufferHandle::checksum() {
  auto v = validate_alive(); if (!v.ok()) return Error(v.error());
  if (ctl_->rec.domain == MemoryDomain::DEVICE) return Error(ErrorCode::unsupported_capability, "checksum: device content requires a host round-trip");
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  const void* p = ctl_->rec.backing.host_map ? ctl_->rec.backing.host_map : ctl_->rec.backing.pointer;
  if (!p) return Error(ErrorCode::state_invalid, "checksum: no host backing");
  ++rt_->counters().integrity_checks;
  return ok(crc32c(p, static_cast<std::size_t>(ctl_->rec.size)));
}

Status BufferHandle::copy_to(void* dst, std::uint64_t dst_offset, std::uint64_t len) {
  auto v = ensure_valid(AccessMode::READ); if (!v.ok()) return Error(v.error());
  if (!dst) return Error(ErrorCode::invalid_argument, "copy_to: null dst");
  {
    std::lock_guard<std::mutex> lk(ctl_->mtx);
    if (dst_offset > ctl_->rec.size || len > ctl_->rec.size - dst_offset) return Error(ErrorCode::bounds_error, "copy_to: length exceeds buffer");
    if (!range_in_bounds(0, len, ctl_->rec.committed)) return Error(ErrorCode::bounds_error, "copy_to: length exceeds backing");
  }
  const void* src = ctl_->rec.backing.host_map ? ctl_->rec.backing.host_map : ctl_->rec.backing.pointer;
  bool src_dev = ctl_->rec.domain == MemoryDomain::DEVICE;
  auto r = internal::copy_memory(static_cast<char*>(dst) + dst_offset, src, len, false, src_dev);
  if (!r.ok()) return r;
  ++rt_->counters().copy_count; rt_->counters().bytes_copied += len;
  return ok_status();
}

Status BufferHandle::copy_from(const void* src, std::uint64_t src_offset, std::uint64_t len) {
  auto v = ensure_valid(AccessMode::WRITE); if (!v.ok()) return Error(v.error());
  if (!src) return Error(ErrorCode::invalid_argument, "copy_from: null src");
  {
    std::lock_guard<std::mutex> lk(ctl_->mtx);
    if (len > ctl_->rec.size) return Error(ErrorCode::bounds_error, "copy_from: length exceeds buffer");
    if (!range_in_bounds(0, len, ctl_->rec.committed)) return Error(ErrorCode::bounds_error, "copy_from: length exceeds backing");
  }
  void* dst = ctl_->rec.backing.host_map ? ctl_->rec.backing.host_map : ctl_->rec.backing.pointer;
  bool dst_dev = ctl_->rec.domain == MemoryDomain::DEVICE;
  auto r = internal::copy_memory(dst, static_cast<const char*>(src) + src_offset, len, dst_dev, false);
  if (!r.ok()) return r;
  ++rt_->counters().copy_count; rt_->counters().bytes_copied += len;
  return ok_status();
}

Result<void*> BufferHandle::device_pointer() const {
  auto v = validate_alive(); if (!v.ok()) return Error(v.error());
  if (ctl_->rec.domain != MemoryDomain::DEVICE) return Error(ErrorCode::invalid_argument, "device_pointer: not a device buffer");
  return ok(ctl_->rec.backing.pointer);
}

const void* BufferHandle::host_data() const {
  if (!ctl_) return nullptr;
  std::lock_guard<std::mutex> lk(ctl_->mtx);
  return ctl_->rec.backing.host_map ? ctl_->rec.backing.host_map : ctl_->rec.backing.pointer;
}

// ---------------------------------------------------------------------------
// BufferLease
// ---------------------------------------------------------------------------
BufferLease::BufferLease(std::shared_ptr<internal::RuntimeImpl> rt, std::shared_ptr<internal::Control> ctl, BufferGeneration gen, LeaseKind kind)
  : rt_(std::move(rt)), ctl_(std::move(ctl)), gen_(gen), kind_(kind) {}
BufferLease::~BufferLease() { release(); }
BufferLease::BufferLease(BufferLease&& o) noexcept { *this = std::move(o); }
BufferLease& BufferLease::operator=(BufferLease&& o) noexcept {
  if (this != &o) { release(); rt_=std::move(o.rt_); ctl_=std::move(o.ctl_); gen_=o.gen_; kind_=o.kind_; o.ctl_.reset(); }
  return *this;
}
void BufferLease::release() {
  if (!ctl_) return;
  auto ctl = ctl_; auto rt = rt_; auto kind = kind_;
  ctl_.reset();
  if (rt && ctl) {
    {
      std::lock_guard<std::mutex> lk(ctl->mtx);
      if (kind == LeaseKind::READ && ctl->read_leases > 0) --ctl->read_leases;
      else if (kind == LeaseKind::WRITE && ctl->write_leases > 0) --ctl->write_leases;
      else if (kind == LeaseKind::EXCLUSIVE && ctl->exclusive_leases > 0) --ctl->exclusive_leases;
      if (ctl->read_leases == 0 && ctl->write_leases == 0 && ctl->exclusive_leases == 0 && ctl->rec.state == BufferState::IN_USE) ctl->rec.state = BufferState::ALLOCATED;
    }
    rt->drop_ref(ctl.get());
  }
}
BufferId BufferLease::id() const { return ctl_ ? ctl_->rec.id : BufferId{}; }
BufferGeneration BufferLease::generation() const { return gen_; }

// ---------------------------------------------------------------------------
// BufferMap
// ---------------------------------------------------------------------------
BufferMap::BufferMap(std::shared_ptr<internal::RuntimeImpl> rt, std::shared_ptr<internal::Control> ctl, BufferGeneration gen, AccessMode access, void* data)
  : rt_(std::move(rt)), ctl_(std::move(ctl)), gen_(gen), access_(access), data_(data) {}
BufferMap::~BufferMap() { release(); }
BufferMap::BufferMap(BufferMap&& o) noexcept { *this = std::move(o); }
BufferMap& BufferMap::operator=(BufferMap&& o) noexcept {
  if (this != &o) { release(); rt_=std::move(o.rt_); ctl_=std::move(o.ctl_); gen_=o.gen_; access_=o.access_; data_=o.data_; o.ctl_.reset(); }
  return *this;
}
void BufferMap::release() {
  if (!ctl_) return;
  auto ctl = ctl_; auto rt = rt_; ctl_.reset(); data_ = nullptr;
  if (rt && ctl) {
    {
      std::lock_guard<std::mutex> lk(ctl->mtx);
      if (ctl->rec.map_count > 0) --ctl->rec.map_count;
      if (ctl->rec.map_count == 0 && ctl->rec.state == BufferState::MAPPED) ctl->rec.state = BufferState::ALLOCATED;
      if (rt && ctl->rec.backing.host_map) { auto* dom = rt->domain(ctl->rec.domain); if (dom) dom->unmap(ctl->rec.backing, ctl->rec.backing.host_map); }
    }
    ++rt->counters().unmap_count;
    rt->drop_ref(ctl.get());
  }
}
BufferId BufferMap::id() const { return ctl_ ? ctl_->rec.id : BufferId{}; }
BufferGeneration BufferMap::generation() const { return gen_; }

// ---------------------------------------------------------------------------
// BufferView
// ---------------------------------------------------------------------------
BufferView::BufferView(std::shared_ptr<internal::RuntimeImpl> rt, std::shared_ptr<internal::Control> ctl, BufferGeneration gen, std::uint64_t off, std::uint64_t len, AccessMode access, void* data)
  : rt_(std::move(rt)), ctl_(std::move(ctl)), gen_(gen), off_(off), len_(len), access_(access), data_(data) {}
BufferView::~BufferView() { release(); }
BufferView::BufferView(BufferView&& o) noexcept { *this = std::move(o); }
BufferView& BufferView::operator=(BufferView&& o) noexcept {
  if (this != &o) { release(); rt_=std::move(o.rt_); ctl_=std::move(o.ctl_); gen_=o.gen_; off_=o.off_; len_=o.len_; access_=o.access_; data_=o.data_; o.ctl_.reset(); }
  return *this;
}
void BufferView::release() {
  if (!ctl_) return;
  auto ctl = ctl_; auto rt = rt_; ctl_.reset(); data_ = nullptr;
  if (rt && ctl) rt->drop_ref(ctl.get());
}
BufferId BufferView::parent_id() const { return ctl_ ? ctl_->rec.id : BufferId{}; }
BufferGeneration BufferView::parent_generation() const { return gen_; }

}  // namespace unified_buffer