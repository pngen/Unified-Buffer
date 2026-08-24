#pragma once
#include "unified_buffer/core/result.hpp"
#include "unified_buffer/core/types.hpp"
#include "unified_buffer/core/capabilities.hpp"
#include <cstdint>
#include <memory>

namespace unified_buffer {

// A native allocation as produced by a memory domain backend.  For CPU-
// accessible domains (HOST, PINNED_HOST, SHARED_HOST, MMAP_STORAGE) both
// `pointer` and `host_map` refer to the same CPU address.  For a DEVICE
// allocation, `pointer` is the raw device pointer and `host_map` is null
// (device memory is not CPU-addressable in general).
struct NativeAllocation {
  void* pointer = nullptr;          // native pointer (device ptr for DEVICE)
  void* host_map = nullptr;         // CPU-mapped alias, if any
  std::uint64_t size = 0;           // requested payload bytes
  std::uint64_t committed = 0;      // bytes actually committed to the backing
  std::uint64_t alignment = 0;
  DeviceId device;                  // which device / backend
  MemoryDomain domain = MemoryDomain::HOST;
  BackendId backend = BackendId::HOST_MALLOC;
  std::shared_ptr<void> state;      // opaque backend-owned state (kept alive)
};

// Per-domain capacity and usage accounting.
struct CapacityInfo {
  std::uint64_t total_capacity = 0;  // backend-reported / configured total
  std::uint64_t reserved = 0;
  std::uint64_t committed = 0;
  std::uint64_t allocated = 0;
  std::uint64_t pooled = 0;
  std::uint64_t idle_pooled = 0;
  std::uint64_t mapped = 0;
  std::uint64_t pending = 0;
  std::uint64_t reclaimable_pooled = 0;
  std::uint64_t free = 0;
  std::uint64_t peak = 0;
  std::uint64_t allocation_count = 0;
};

// Backend-neutral memory-domain interface.  A concrete backend implements one
// or more domains.  Backends advertise capabilities explicitly; the runtime
// rejects unsupported operations rather than silently emulating them.
class IDomain {
 public:
  virtual ~IDomain() = default;

  virtual BackendId backend() const = 0;
  virtual MemoryDomain domain() const = 0;
  virtual const Capabilities& capabilities() const = 0;

  // Allocate `bytes` bytes aligned to `alignment`.  Returns a NativeAllocation.
  virtual Result<NativeAllocation> allocate(std::uint64_t bytes, std::uint64_t alignment) = 0;
  // Free an allocation produced by this domain.  Returns bytes released.
  virtual Result<std::uint64_t> free(NativeAllocation&) = 0;

  virtual Status zero(NativeAllocation&, std::uint64_t offset, std::uint64_t len) = 0;
  virtual Status synchronize(NativeAllocation&) = 0;
  virtual Status flush(NativeAllocation&) = 0;
  virtual Status verify(NativeAllocation&, std::uint64_t offset, std::uint64_t len) = 0;

  // Map / unmap a CPU-accessible view.  For device memory these fail with
  // unsupported_capability unless the domain supports CPU mapping.
  virtual Result<void*> map(NativeAllocation&, AccessMode) = 0;
  virtual Status unmap(NativeAllocation&, void* p) = 0;

  // Contiguous copy within this domain.  Cross-domain copies are composed by
  // the runtime.
  virtual Status copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off,
                                const NativeAllocation& src, std::uint64_t src_off,
                                std::uint64_t len) = 0;

  virtual Result<CapacityInfo> capacity() const = 0;
  virtual Result<std::uint64_t> preferred_alignment() const = 0;
  virtual Result<std::uint64_t> default_alignment() const = 0;

  // Serialize / reconstruct an external, backend-specific handle (used for
  // SHARED_HOST and MMAP_STORAGE export/import).  Other domains report
  // unsupported_capability by default.
  virtual Result<std::string> export_handle(const NativeAllocation&) const {
    return Error(ErrorCode::unsupported_capability, "domain: no external handle");
  }
  virtual Result<NativeAllocation> import_handle(const std::string&, std::uint64_t, std::uint64_t) const {
    return Error(ErrorCode::unsupported_capability, "domain: cannot import handle");
  }
};

} // namespace unified_buffer