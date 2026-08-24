#include "unified_buffer/backends/file_backend.hpp"
#include "unified_buffer/core/checked_math.hpp"
#include <cstring>
#include <string>
#include <atomic>
#if defined(_WIN32)
#include <windows.h>
#include <fileapi.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace unified_buffer {

struct FileState {
  std::uint64_t size = 0;
  std::string path;
#if defined(_WIN32)
  HANDLE file = nullptr;
  HANDLE mapping = nullptr;
  void* view = nullptr;
  ~FileState() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    if (file) CloseHandle(file);
  }
#elif defined(__linux__)
  int fd = -1;
  void* view = nullptr;
  ~FileState() {
    if (view) munmap(view, size);
    if (fd >= 0) close(fd);
  }
#endif
};

FileBackedDomain::FileBackedDomain(std::uint64_t cap, const std::string& base_dir) : cap_(cap), base_dir_(base_dir) { init(); }

void FileBackedDomain::init() {
  if (base_dir_.empty()) {
#if defined(_WIN32)
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n > 0) base_dir_ = std::string(tmp, n);
    else base_dir_ = ".";
#else
    const char* t = getenv("TMPDIR"); if (!t) t = "/tmp";
    base_dir_ = t;
#endif
  }
  caps_.set(Capability::HOST_READABLE);
  caps_.set(Capability::HOST_WRITABLE);
  caps_.set(Capability::CPU_MAPPABLE);
  caps_.set(Capability::EXPORTABLE);
  caps_.set(Capability::IMPORTABLE);
  caps_.set(Capability::PERSISTENT);
  caps_.set(Capability::SUPPORTS_EXPLICIT_FLUSH);
  caps_.set(Capability::SUPPORTS_EXPLICIT_INVALIDATE);
}

static std::uint64_t file_counter() { static std::atomic<std::uint64_t> c{0}; return c.fetch_add(1); }

#if defined(_WIN32)
Result<NativeAllocation> FileBackedDomain::allocate(std::uint64_t bytes, std::uint64_t alignment) {
  if (bytes == 0) return Error(ErrorCode::invalid_argument, "file alloc: zero size");
  std::string path = base_dir_ + "\\ubf_" + std::to_string(file_counter()) + ".bin";
  HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return Error(ErrorCode::backend_failure, "file alloc: CreateFile failed");
  LARGE_INTEGER sz; sz.QuadPart = static_cast<LONGLONG>(bytes);
  if (!SetFilePointerEx(f, sz, nullptr, FILE_BEGIN)) { CloseHandle(f); return Error(ErrorCode::backend_failure, "file alloc: seek failed"); }
  if (!SetEndOfFile(f)) { CloseHandle(f); return Error(ErrorCode::out_of_capacity, "file alloc: truncate failed"); }
  HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, 0, nullptr);
  if (!m) { CloseHandle(f); return Error(ErrorCode::mapping_failure, "file alloc: CreateFileMapping failed"); }
  void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!view) { CloseHandle(m); CloseHandle(f); return Error(ErrorCode::mapping_failure, "file alloc: MapViewOfFile failed"); }
  auto st = std::make_shared<FileState>();
  st->file = f; st->mapping = m; st->view = view; st->size = bytes; st->path = path;
  NativeAllocation a;
  a.pointer = view; a.host_map = view; a.size = bytes; a.committed = bytes;
  a.alignment = alignment ? alignment : 64;
  a.domain = MemoryDomain::MMAP_STORAGE; a.backend = BackendId::FILE_BACKED;
  a.device = DeviceId{BackendId::FILE_BACKED, -1};
  a.state = st;
  return ok(std::move(a));
}

Result<std::uint64_t> FileBackedDomain::free(NativeAllocation& a) {
  if (!a.pointer) return Error(ErrorCode::invalid_argument, "file free: null");
  const auto bytes = a.committed;
  a.state.reset();
  a = NativeAllocation{};
  return ok(bytes);
}

Result<std::string> FileBackedDomain::export_handle(const NativeAllocation& a) const {
  auto st = std::static_pointer_cast<FileState>(a.state);
  if (!st) return Error(ErrorCode::state_invalid, "file export: no state");
  return ok(st->path);
}

bool path_is_beneath(const std::string& base, const std::string& path) {
  if (path.rfind(base, 0) != 0) return false;
  return true;
}

Result<NativeAllocation> FileBackedDomain::import_handle(const std::string& path, std::uint64_t size, std::uint64_t alignment) const {
  if (path.empty()) return Error(ErrorCode::import_failure, "file import: empty path");
  if (path.size() > 1024) return Error(ErrorCode::import_failure, "file import: path too long");
  // Path traversal protection: must be within the configured base dir and not contain .. segments.
  if (path.find("\\..\\") != std::string::npos || path.find("..") != std::string::npos)
    return Error(ErrorCode::import_failure, "file import: traversal rejected");
  if (!path_is_beneath(base_dir_, path)) return Error(ErrorCode::import_failure, "file import: outside base dir");
  HANDLE f = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return Error(ErrorCode::import_failure, "file import: open failed");
  LARGE_INTEGER fs; if (!GetFileSizeEx(f, &fs)) { CloseHandle(f); return Error(ErrorCode::import_failure, "file import: stat failed"); }
  if (static_cast<std::uint64_t>(fs.QuadPart) < size) { CloseHandle(f); return Error(ErrorCode::import_failure, "file import: file too small"); }
  HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, 0, 0, nullptr);
  if (!m) { CloseHandle(f); return Error(ErrorCode::mapping_failure, "file import: mapping failed"); }
  void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!view) { CloseHandle(m); CloseHandle(f); return Error(ErrorCode::mapping_failure, "file import: map view failed"); }
  auto st = std::make_shared<FileState>();
  st->file = f; st->mapping = m; st->view = view; st->size = size; st->path = path;
  NativeAllocation a;
  a.pointer = view; a.host_map = view; a.size = size; a.committed = size;
  a.alignment = alignment ? alignment : 64;
  a.domain = MemoryDomain::MMAP_STORAGE; a.backend = BackendId::FILE_BACKED; a.state = st;
  return ok(std::move(a));
}
#else
Result<NativeAllocation> FileBackedDomain::allocate(std::uint64_t, std::uint64_t) {
  return Error(ErrorCode::unsupported_domain, "file-backend: POSIX backend not compiled on this host");
}
Result<std::uint64_t> FileBackedDomain::free(NativeAllocation& a) { a.state.reset(); a = NativeAllocation{}; return ok(a.committed); }
Result<std::string> FileBackedDomain::export_handle(const NativeAllocation&) const { return Error(ErrorCode::unsupported_capability, "file export: unsupported"); }
Result<NativeAllocation> FileBackedDomain::import_handle(const std::string&, std::uint64_t, std::uint64_t) const { return Error(ErrorCode::unsupported_capability, "file import: unsupported"); }
#endif

Status FileBackedDomain::zero(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (offset > a.committed || len > a.committed - offset) return Error(ErrorCode::bounds_error, "file zero: oob");
  std::memset(static_cast<char*>(a.host_map) + offset, 0, static_cast<std::size_t>(len));
  return ok_status();
}
Status FileBackedDomain::synchronize(NativeAllocation&) { return ok_status(); }
Status FileBackedDomain::flush(NativeAllocation& a) {
#if defined(_WIN32)
  if (a.host_map && !FlushViewOfFile(a.host_map, 0)) return Error(ErrorCode::backend_failure, "file flush: FlushViewOfFile failed");
  auto st = std::static_pointer_cast<FileState>(a.state);
  if (st && st->file && !FlushFileBuffers(st->file)) return Error(ErrorCode::backend_failure, "file flush: FlushFileBuffers failed");
#endif
  return ok_status();
}
Status FileBackedDomain::verify(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (!a.pointer) return Error(ErrorCode::state_invalid, "file verify: no backing");
  if (offset > a.committed || len > a.committed - offset) return Error(ErrorCode::bounds_error, "file verify: oob");
  volatile unsigned char* p = static_cast<volatile unsigned char*>(a.host_map) + offset;
  (void)p[0];
  return ok_status();
}
Result<void*> FileBackedDomain::map(NativeAllocation& a, AccessMode) { return ok(a.host_map); }
Status FileBackedDomain::unmap(NativeAllocation&, void*) { return ok_status(); }
Status FileBackedDomain::copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off, const NativeAllocation& src, std::uint64_t src_off, std::uint64_t len) {
  if (dst_off > dst.committed || len > dst.committed - dst_off) return Error(ErrorCode::bounds_error, "file copy: dst oob");
  if (src_off > src.committed || len > src.committed - src_off) return Error(ErrorCode::bounds_error, "file copy: src oob");
  std::memmove(static_cast<char*>(dst.host_map) + dst_off, static_cast<const char*>(src.host_map) + src_off, static_cast<std::size_t>(len));
  return ok_status();
}
Result<CapacityInfo> FileBackedDomain::capacity() const { CapacityInfo ci; ci.total_capacity = cap_; return ok(ci); }
Result<std::uint64_t> FileBackedDomain::preferred_alignment() const { return ok(std::uint64_t{4096}); }
Result<std::uint64_t> FileBackedDomain::default_alignment() const { return ok(std::uint64_t{64}); }

}  // namespace unified_buffer
