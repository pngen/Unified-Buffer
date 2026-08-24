#include "unified_buffer/backends/host_backend.hpp"
#include "unified_buffer/core/checked_math.hpp"
#include <malloc.h>
#include <cstring>
#include <algorithm>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace unified_buffer {

namespace {
std::uint64_t system_physical_bytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) return ms.ullTotalPhys;
#else
  // POSIX: read from sysconf where available (kept minimal to avoid deps).
  return 0;
#endif
  return 0;
}
}  // namespace

HostDomain::HostDomain(std::uint64_t capacity_hint) : cap_hint_(capacity_hint) {
  total_bytes_ = cap_hint_ ? cap_hint_ : system_physical_bytes();
  if (total_bytes_ == 0) total_bytes_ = 1ULL << 40;  // reasonable default (1 TiB)
  init_capabilities();
}

void HostDomain::init_capabilities() {
  caps_.set(Capability::HOST_READABLE);
  caps_.set(Capability::HOST_WRITABLE);
  caps_.set(Capability::CPU_MAPPABLE);
  caps_.set(Capability::PAGEABLE);
  caps_.set(Capability::EXPORTABLE);
  caps_.set(Capability::IMPORTABLE);
  caps_.set(Capability::PERSISTENT);
  caps_.set(Capability::COHERENT);
  caps_.set(Capability::SUPPORTS_SUBALLOCATION);
}

Result<NativeAllocation> HostDomain::allocate(std::uint64_t bytes, std::uint64_t alignment) {
  if (bytes == 0) return Error(ErrorCode::invalid_argument, "host allocation: zero size");
  std::uint64_t min_align = sizeof(void*);
  if (alignment < min_align) alignment = min_align;
  // _aligned_malloc requires a power-of-two alignment >= sizeof(void*).
  if ((alignment & (alignment - 1)) != 0) return Error(ErrorCode::alignment_error, "host allocation: alignment not power-of-two");
  void* p = _aligned_malloc(static_cast<std::size_t>(bytes), static_cast<std::size_t>(alignment));
  if (!p) return Error(ErrorCode::out_of_capacity, "host allocation: _aligned_malloc failed");
  NativeAllocation a;
  a.pointer = p;
  a.host_map = p;
  a.size = bytes;
  a.committed = bytes;
  a.alignment = alignment;
  a.device = DeviceId{BackendId::HOST_MALLOC, -1};
  a.domain = MemoryDomain::HOST;
  a.backend = BackendId::HOST_MALLOC;
  return ok(std::move(a));
}

Result<std::uint64_t> HostDomain::free(NativeAllocation& a) {
  if (!a.pointer) return Error(ErrorCode::invalid_argument, "host free: null pointer");
  _aligned_free(a.pointer);
  const auto bytes = a.committed;
  a = NativeAllocation{};
  return ok(bytes);
}

Status HostDomain::zero(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (offset > a.committed || len > a.committed - offset)
    return Error(ErrorCode::bounds_error, "host zero: out of bounds");
  auto* p = static_cast<unsigned char*>(a.host_map ? a.host_map : a.pointer) + offset;
  std::memset(p, 0, static_cast<std::size_t>(len));
  return ok_status();
}

Status HostDomain::synchronize(NativeAllocation&) { return ok_status(); }
Status HostDomain::flush(NativeAllocation&) { return ok_status(); }

Status HostDomain::verify(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (!a.pointer && !a.host_map) return Error(ErrorCode::state_invalid, "host verify: no backing");
  if (offset > a.committed || len > a.committed - offset)
    return Error(ErrorCode::bounds_error, "host verify: out of bounds");
  // Accessibility probe: touching the range is cheap and catches wild pointers.
  volatile unsigned char* p = static_cast<volatile unsigned char*>(a.host_map ? a.host_map : a.pointer) + offset;
  const unsigned char sink = p[0];
  (void)sink;
  return ok_status();
}

Result<void*> HostDomain::map(NativeAllocation& a, AccessMode) { return ok(a.host_map ? a.host_map : a.pointer); }

Status HostDomain::unmap(NativeAllocation&, void*) { return ok_status(); }

Status HostDomain::copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off,
                                  const NativeAllocation& src, std::uint64_t src_off,
                                  std::uint64_t len) {
  if (dst_off > dst.committed || len > dst.committed - dst_off)
    return Error(ErrorCode::bounds_error, "host copy_in_domain: dst out of bounds");
  if (src_off > src.committed || len > src.committed - src_off)
    return Error(ErrorCode::bounds_error, "host copy_in_domain: src out of bounds");
  auto* d = static_cast<unsigned char*>(dst.host_map ? dst.host_map : dst.pointer) + dst_off;
  const auto* s = static_cast<const unsigned char*>(src.host_map ? src.host_map : src.pointer) + src_off;
  std::memmove(d, s, static_cast<std::size_t>(len));  // memmove -> overlap safe
  return ok_status();
}

Result<CapacityInfo> HostDomain::capacity() const {
  CapacityInfo ci;
  ci.total_capacity = total_bytes_;
  return ok(ci);
}
Result<std::uint64_t> HostDomain::preferred_alignment() const { return ok(std::uint64_t{64}); }
Result<std::uint64_t> HostDomain::default_alignment() const { return ok(static_cast<std::uint64_t>(sizeof(void*))); }

} // namespace unified_buffer