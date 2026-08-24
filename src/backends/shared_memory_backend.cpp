#include "unified_buffer/backends/shared_memory_backend.hpp"
#include "unified_buffer/core/checked_math.hpp"
#include <cstring>
#include <string>
#include <atomic>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
namespace {
std::uint64_t unique_counter() { static std::atomic<std::uint64_t> c{0}; return c.fetch_add(1); }
std::string make_unique_name() {
  static const std::uint64_t seed = std::chrono::steady_clock::now().time_since_epoch().count();
  return "UnifiedBuffer_" + std::to_string(seed ^ unique_counter()) + "_" + std::to_string(unique_counter());
}
}
#elif defined(__linux__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace unified_buffer {

// Backend state that owns the platform mapping / handle.
struct SharedState {
  std::uint64_t size = 0;
#if defined(_WIN32)
  HANDLE file_map = nullptr;
  void* view = nullptr;
  std::string name;
  ~SharedState() {
    if (view) UnmapViewOfFile(view);
    if (file_map) CloseHandle(file_map);
  }
#elif defined(__linux__)
  int fd = -1;
  std::string name;
  void* view = nullptr;
  ~SharedState() {
    if (view) munmap(view, size);
    if (fd >= 0) close(fd);
  }
#endif
};

SharedMemoryDomain::SharedMemoryDomain(std::uint64_t cap) : cap_(cap) { init(); }

void SharedMemoryDomain::init() {
  caps_.set(Capability::HOST_READABLE);
  caps_.set(Capability::HOST_WRITABLE);
  caps_.set(Capability::CPU_MAPPABLE);
  caps_.set(Capability::EXPORTABLE);
  caps_.set(Capability::IMPORTABLE);
  caps_.set(Capability::SHAREABLE_ACROSS_PROCESS);
  caps_.set(Capability::COHERENT);
}

#if defined(_WIN32)
Result<NativeAllocation> SharedMemoryDomain::allocate(std::uint64_t bytes, std::uint64_t alignment) {
  if (bytes == 0) return Error(ErrorCode::invalid_argument, "shared alloc: zero size");
  // Unique mapping name.
  std::string name = make_unique_name();
  HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                static_cast<DWORD>(bytes), name.c_str());
  if (!h) return Error(ErrorCode::out_of_capacity, "shared alloc: CreateFileMapping failed");
  void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!view) { CloseHandle(h); return Error(ErrorCode::mapping_failure, "shared alloc: MapViewOfFile failed"); }
  auto st = std::make_shared<SharedState>();
  st->file_map = h;
  st->view = view;
  st->size = bytes;
  st->name = name;
  NativeAllocation a;
  a.pointer = view;
  a.host_map = view;
  a.size = bytes;
  a.committed = bytes;
  a.alignment = alignment ? alignment : 64;
  a.domain = MemoryDomain::SHARED_HOST;
  a.backend = BackendId::WIN_SHARED_FILE;
  a.device = DeviceId{BackendId::WIN_SHARED_FILE, -1};
  a.state = st;
  return ok(std::move(a));
}

Result<std::uint64_t> SharedMemoryDomain::free(NativeAllocation& a) {
  if (!a.pointer) return Error(ErrorCode::invalid_argument, "shared free: null");
  const auto bytes = a.committed;
  a.state.reset();      // unmaps + closes
  a = NativeAllocation{};
  return ok(bytes);
}

Result<std::string> SharedMemoryDomain::export_handle(const NativeAllocation& a) const {
  auto st = std::static_pointer_cast<SharedState>(a.state);
  if (!st) return Error(ErrorCode::state_invalid, "shared export: no state");
  return ok(st->name);
}

Result<NativeAllocation> SharedMemoryDomain::import_handle(const std::string& name, std::uint64_t size, std::uint64_t alignment) const {
  if (name.empty()) return Error(ErrorCode::import_failure, "shared import: empty name");
  if (name.size() > 512) return Error(ErrorCode::import_failure, "shared import: name too long");
  // Reject path separators / traversal in the handle name.
  if (name.find_first_of("\\/") != std::string::npos) return Error(ErrorCode::import_failure, "shared import: illegal name");
  HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!h) return Error(ErrorCode::import_failure, "shared import: OpenFileMapping failed");
  void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!view) { CloseHandle(h); return Error(ErrorCode::mapping_failure, "shared import: MapViewOfFile failed"); }
  auto st = std::make_shared<SharedState>();
  st->file_map = h;
  st->view = view;
  st->size = size;
  st->name = name;
  NativeAllocation a;
  a.pointer = view;
  a.host_map = view;
  a.size = size;
  a.committed = size;
  a.alignment = alignment ? alignment : 64;
  a.domain = MemoryDomain::SHARED_HOST;
  a.backend = BackendId::WIN_SHARED_FILE;
  a.state = st;
  return ok(std::move(a));
}
#else
Result<NativeAllocation> SharedMemoryDomain::allocate(std::uint64_t, std::uint64_t) {
  return Error(ErrorCode::unsupported_domain, "shared memory: POSIX backend not compiled on this host");
}
Result<std::uint64_t> SharedMemoryDomain::free(NativeAllocation& a) { a.state.reset(); a = NativeAllocation{}; return ok(a.committed); }
Result<std::string> SharedMemoryDomain::export_handle(const NativeAllocation&) const { return Error(ErrorCode::unsupported_capability, "shared export: unsupported"); }
Result<NativeAllocation> SharedMemoryDomain::import_handle(const std::string&, std::uint64_t, std::uint64_t) const { return Error(ErrorCode::unsupported_capability, "shared import: unsupported"); }
#endif

Status SharedMemoryDomain::zero(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (offset > a.committed || len > a.committed - offset) return Error(ErrorCode::bounds_error, "shared zero: out of bounds");
  std::memset(static_cast<char*>(a.host_map) + offset, 0, static_cast<std::size_t>(len));
  return ok_status();
}
Status SharedMemoryDomain::synchronize(NativeAllocation&) { return ok_status(); }
Status SharedMemoryDomain::flush(NativeAllocation&) { return ok_status(); }
Status SharedMemoryDomain::verify(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (!a.pointer) return Error(ErrorCode::state_invalid, "shared verify: no backing");
  if (offset > a.committed || len > a.committed - offset) return Error(ErrorCode::bounds_error, "shared verify: out of bounds");
  volatile unsigned char* p = static_cast<volatile unsigned char*>(a.host_map) + offset;
  (void)p[0];
  return ok_status();
}
Result<void*> SharedMemoryDomain::map(NativeAllocation& a, AccessMode) { return ok(a.host_map); }
Status SharedMemoryDomain::unmap(NativeAllocation&, void*) { return ok_status(); }
Status SharedMemoryDomain::copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off, const NativeAllocation& src, std::uint64_t src_off, std::uint64_t len) {
  if (dst_off > dst.committed || len > dst.committed - dst_off) return Error(ErrorCode::bounds_error, "shared copy: dst oob");
  if (src_off > src.committed || len > src.committed - src_off) return Error(ErrorCode::bounds_error, "shared copy: src oob");
  std::memmove(static_cast<char*>(dst.host_map) + dst_off, static_cast<const char*>(src.host_map) + src_off, static_cast<std::size_t>(len));
  return ok_status();
}
Result<CapacityInfo> SharedMemoryDomain::capacity() const { CapacityInfo ci; ci.total_capacity = cap_; return ok(ci); }
Result<std::uint64_t> SharedMemoryDomain::preferred_alignment() const { return ok(std::uint64_t{4096}); }
Result<std::uint64_t> SharedMemoryDomain::default_alignment() const { return ok(std::uint64_t{64}); }

}  // namespace unified_buffer