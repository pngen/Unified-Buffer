#ifdef UNIFIED_BUFFER_HAS_CUDA
#include "unified_buffer/backends/cuda_backend.hpp"
#include "unified_buffer/core/checked_math.hpp"
#include <cuda_runtime.h>
#include <cstring>

namespace unified_buffer {
namespace {
Error cu_err(cudaError_t e, const char* op) {
  if (e == cudaSuccess) return Error();
  return Error(ErrorCode::backend_failure, std::string(op) + ": " + cudaGetErrorString(e));
}
}  // namespace

namespace cuda_support {

bool available() noexcept {
  static const bool ok = ([]() {
    int count = 0;
    cudaError_t e = cudaGetDeviceCount(&count);
    return e == cudaSuccess && count > 0;
  })();
  return ok;
}

int device_count() {
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess) return 0;
  return count;
}

Result<CudaDeviceInfo> device_info(int index) {
  if (index < 0) return Error(ErrorCode::invalid_device, "negative device index");
  cudaDeviceProp prop{};
  if (cudaGetDeviceProperties(&prop, index) != cudaSuccess)
    return Error(ErrorCode::device_unavailable, "cudaGetDeviceProperties failed");
  CudaDeviceInfo info;
  info.index = index;
  info.name = prop.name;
  info.compute_major = prop.major;
  info.compute_minor = prop.minor;
  info.runtime_version = prop.major * 1000 + prop.minor * 10;
  int driver = 0;
  if (cudaDriverGetVersion(&driver) == cudaSuccess) info.driver_version = driver;
  size_t free_b = 0, total_b = 0;
  if (cudaMemGetInfo(&free_b, &total_b) == cudaSuccess) { info.total_memory = total_b; info.free_memory = free_b; }
  info.multiprocessor_count = prop.multiProcessorCount;
  return ok(info);
}

Result<std::uint64_t> device_total_memory(int) {
  size_t free_b = 0, total_b = 0;
  if (cudaMemGetInfo(&free_b, &total_b) != cudaSuccess)
    return Error(ErrorCode::backend_failure, "cudaMemGetInfo failed");
  return ok(static_cast<std::uint64_t>(total_b));
}

Result<std::uint64_t> device_free_memory(int) {
  size_t free_b = 0, total_b = 0;
  if (cudaMemGetInfo(&free_b, &total_b) != cudaSuccess)
    return Error(ErrorCode::backend_failure, "cudaMemGetInfo failed");
  return ok(static_cast<std::uint64_t>(free_b));
}

Status memcpy_cross(void* dst, const void* src, std::size_t bytes, cudaMemcpyKind kind) {
  if (bytes == 0) return ok_status();
  cudaError_t e = cudaMemcpy(dst, src, bytes, kind);
  if (e != cudaSuccess) return Error(ErrorCode::backend_failure, std::string("cudaMemcpy: ") + cudaGetErrorString(e));
  return ok_status();
}

Status device_sync() {
  cudaError_t e = cudaDeviceSynchronize();
  if (e != cudaSuccess) return Error(ErrorCode::backend_failure, std::string("cudaDeviceSynchronize: ") + cudaGetErrorString(e));
  return ok_status();
}

}  // namespace cuda_support

// ---------------------------------------------------------------------------
// CudaDeviceDomain
// ---------------------------------------------------------------------------
CudaDeviceDomain::CudaDeviceDomain() {
  device_count_ = cuda_support::device_count();
  if (device_count_ > 0) cudaGetDevice(&active_device_);
  caps_.set(Capability::DIRECTLY_DMA_CAPABLE);
  caps_.set(Capability::HOST_READABLE);
  caps_.set(Capability::HOST_WRITABLE);
  caps_.set(Capability::DEVICE_READABLE);
  caps_.set(Capability::DEVICE_WRITABLE);
  caps_.set(Capability::ASYNC_COPYABLE);
  caps_.set(Capability::SUPPORTS_EXTERNAL_HANDLE);
}

CudaDeviceDomain::~CudaDeviceDomain() {
  if (device_count_ > 0) {
    cudaError_t e = cudaDeviceSynchronize();
    if (e != cudaSuccess) { /* best effort on shutdown */ }
  }
}

Result<NativeAllocation> CudaDeviceDomain::allocate(std::uint64_t bytes, std::uint64_t alignment) {
  if (!cuda_support::available()) return Error(ErrorCode::device_unavailable, "no CUDA device");
  if (bytes == 0) return Error(ErrorCode::invalid_argument, "device allocation: zero size");
  // cudaMalloc returns >=256-byte aligned memory.  Honour a reasonable cap.
  if (alignment > 4096) return Error(ErrorCode::alignment_error, "device allocation: alignment too large");
  if ((alignment != 0) && ((alignment & (alignment - 1)) != 0))
    return Error(ErrorCode::alignment_error, "device allocation: alignment not power-of-two");
  if (cudaSetDevice(active_device_) != cudaSuccess)
    return Error(ErrorCode::backend_failure, "cudaSetDevice failed");
  void* p = nullptr;
  cudaError_t e = cudaMalloc(&p, static_cast<std::size_t>(bytes));
  if (e != cudaSuccess) return Error(ErrorCode::out_of_capacity, std::string("cudaMalloc: ") + cudaGetErrorString(e));
  NativeAllocation a;
  a.pointer = p;
  a.host_map = nullptr;
  a.size = bytes;
  a.committed = bytes;
  a.alignment = alignment ? alignment : 256;
  a.device = DeviceId{BackendId::CUDA, active_device_};
  a.domain = MemoryDomain::DEVICE;
  a.backend = BackendId::CUDA;
  return ok(std::move(a));
}

Result<std::uint64_t> CudaDeviceDomain::free(NativeAllocation& a) {
  if (!a.pointer) return Error(ErrorCode::invalid_argument, "device free: null pointer");
  cudaError_t e = cudaFree(a.pointer);
  if (e != cudaSuccess) return Error(ErrorCode::backend_failure, std::string("cudaFree: ") + cudaGetErrorString(e));
  const auto bytes = a.committed;
  a = NativeAllocation{};
  return ok(bytes);
}

Status CudaDeviceDomain::zero(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (!a.pointer) return Error(ErrorCode::state_invalid, "device zero: no backing");
  if (offset > a.committed || len > a.committed - offset)
    return Error(ErrorCode::bounds_error, "device zero: out of bounds");
  cudaError_t e = cudaMemset(static_cast<char*>(a.pointer) + offset, 0, static_cast<std::size_t>(len));
  if (e != cudaSuccess) return Error(ErrorCode::backend_failure, std::string("cudaMemset: ") + cudaGetErrorString(e));
  if (cudaDeviceSynchronize() != cudaSuccess) return Error(ErrorCode::backend_failure, "device zero sync failed");
  return ok_status();
}

Status CudaDeviceDomain::synchronize(NativeAllocation&) { return cuda_support::device_sync(); }
Status CudaDeviceDomain::flush(NativeAllocation&) { return cuda_support::device_sync(); }

Status CudaDeviceDomain::verify(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (!a.pointer) return Error(ErrorCode::state_invalid, "device verify: no backing");
  if (offset > a.committed || len > a.committed - offset)
    return Error(ErrorCode::bounds_error, "device verify: out of bounds");
  cudaPointerAttributes attr{};
  cudaError_t e = cudaPointerGetAttributes(&attr, a.pointer);
  if (e != cudaSuccess) return Error(ErrorCode::integrity_failure, std::string("device verify: not a valid pointer: ") + cudaGetErrorString(e));
  // Self-copy validates the range is readable/writable on the device.
  if (len > 0) {
    e = cudaMemcpy(static_cast<char*>(a.pointer) + offset, static_cast<char*>(a.pointer) + offset, static_cast<std::size_t>(len), cudaMemcpyDeviceToDevice);
    if (e != cudaSuccess) return Error(ErrorCode::integrity_failure, std::string("device verify self-copy: ") + cudaGetErrorString(e));
  }
  return ok_status();
}

Result<void*> CudaDeviceDomain::map(NativeAllocation&, AccessMode) {
  return Error(ErrorCode::unsupported_capability, "device memory is not CPU-mappable");
}

Status CudaDeviceDomain::unmap(NativeAllocation&, void*) {
  return Error(ErrorCode::unsupported_capability, "device memory is not CPU-mappable");
}

Status CudaDeviceDomain::copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off,
                                        const NativeAllocation& src, std::uint64_t src_off,
                                        std::uint64_t len) {
  if (!dst.pointer || !src.pointer) return Error(ErrorCode::state_invalid, "device copy_in_domain: missing backing");
  if (dst_off > dst.committed || len > dst.committed - dst_off)
    return Error(ErrorCode::bounds_error, "device copy_in_domain: dst out of bounds");
  if (src_off > src.committed || len > src.committed - src_off)
    return Error(ErrorCode::bounds_error, "device copy_in_domain: src out of bounds");
  cudaError_t e = cudaMemcpy(static_cast<char*>(dst.pointer) + dst_off,
                            static_cast<const char*>(src.pointer) + src_off,
                            static_cast<std::size_t>(len), cudaMemcpyDeviceToDevice);
  if (e != cudaSuccess) return Error(ErrorCode::backend_failure, std::string("cudaMemcpy D2D: ") + cudaGetErrorString(e));
  if (cudaDeviceSynchronize() != cudaSuccess) return Error(ErrorCode::backend_failure, "D2D sync failed");
  return ok_status();
}

Result<CapacityInfo> CudaDeviceDomain::capacity() const {
  CapacityInfo ci;
  size_t free_b = 0, total_b = 0;
  if (cudaMemGetInfo(&free_b, &total_b) == cudaSuccess) {
    ci.total_capacity = total_b;
    ci.free = free_b;
  }
  return ok(ci);
}
Result<std::uint64_t> CudaDeviceDomain::preferred_alignment() const { return ok(std::uint64_t{256}); }
Result<std::uint64_t> CudaDeviceDomain::default_alignment() const { return ok(std::uint64_t{256}); }

// ---------------------------------------------------------------------------
// CudaPinnedDomain
// ---------------------------------------------------------------------------
CudaPinnedDomain::CudaPinnedDomain() {
  caps_.set(Capability::HOST_READABLE);
  caps_.set(Capability::HOST_WRITABLE);
  caps_.set(Capability::CPU_MAPPABLE);
  caps_.set(Capability::PINNED);
  caps_.set(Capability::DIRECTLY_DMA_CAPABLE);
  caps_.set(Capability::ASYNC_COPYABLE);
}
CudaPinnedDomain::~CudaPinnedDomain() {
  cudaError_t e = cudaDeviceSynchronize();
  if (e != cudaSuccess) { /* best effort */ }
}

Result<NativeAllocation> CudaPinnedDomain::allocate(std::uint64_t bytes, std::uint64_t alignment) {
  if (!cuda_support::available()) return Error(ErrorCode::device_unavailable, "no CUDA device");
  if (bytes == 0) return Error(ErrorCode::invalid_argument, "pinned allocation: zero size");
  void* p = nullptr;
  cudaError_t e = cudaHostAlloc(&p, static_cast<std::size_t>(bytes), cudaHostAllocDefault);
  if (e != cudaSuccess) return Error(ErrorCode::out_of_capacity, std::string("cudaHostAlloc: ") + cudaGetErrorString(e));
  NativeAllocation a;
  a.pointer = p;
  a.host_map = p;
  a.size = bytes;
  a.committed = bytes;
  a.alignment = alignment ? alignment : 256;
  a.device = DeviceId{BackendId::CUDA, 0};
  a.domain = MemoryDomain::PINNED_HOST;
  a.backend = BackendId::CUDA;
  return ok(std::move(a));
}

Result<std::uint64_t> CudaPinnedDomain::free(NativeAllocation& a) {
  if (!a.pointer) return Error(ErrorCode::invalid_argument, "pinned free: null pointer");
  cudaError_t e = cudaFreeHost(a.pointer);
  if (e != cudaSuccess) return Error(ErrorCode::backend_failure, std::string("cudaFreeHost: ") + cudaGetErrorString(e));
  const auto bytes = a.committed;
  a = NativeAllocation{};
  return ok(bytes);
}

Status CudaPinnedDomain::zero(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (offset > a.committed || len > a.committed - offset)
    return Error(ErrorCode::bounds_error, "pinned zero: out of bounds");
  std::memset(static_cast<char*>(a.host_map) + offset, 0, static_cast<std::size_t>(len));
  return ok_status();
}
Status CudaPinnedDomain::synchronize(NativeAllocation&) { return cuda_support::device_sync(); }
Status CudaPinnedDomain::flush(NativeAllocation&) { return cuda_support::device_sync(); }
Status CudaPinnedDomain::verify(NativeAllocation& a, std::uint64_t offset, std::uint64_t len) {
  if (!a.pointer) return Error(ErrorCode::state_invalid, "pinned verify: no backing");
  if (offset > a.committed || len > a.committed - offset)
    return Error(ErrorCode::bounds_error, "pinned verify: out of bounds");
  volatile unsigned char* p = static_cast<volatile unsigned char*>(a.host_map) + offset;
  (void)p[0];
  return ok_status();
}
Result<void*> CudaPinnedDomain::map(NativeAllocation& a, AccessMode) { return ok(a.host_map); }
Status CudaPinnedDomain::unmap(NativeAllocation&, void*) { return ok_status(); }
Status CudaPinnedDomain::copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off,
                                        const NativeAllocation& src, std::uint64_t src_off,
                                        std::uint64_t len) {
  if (dst_off > dst.committed || len > dst.committed - dst_off)
    return Error(ErrorCode::bounds_error, "pinned copy_in_domain: dst out of bounds");
  if (src_off > src.committed || len > src.committed - src_off)
    return Error(ErrorCode::bounds_error, "pinned copy_in_domain: src out of bounds");
  std::memmove(static_cast<char*>(dst.host_map) + dst_off, static_cast<const char*>(src.host_map) + src_off, static_cast<std::size_t>(len));
  return ok_status();
}
Result<CapacityInfo> CudaPinnedDomain::capacity() const {
  CapacityInfo ci;
  size_t free_b = 0, total_b = 0;
  if (cudaMemGetInfo(&free_b, &total_b) == cudaSuccess) ci.total_capacity = total_b;
  return ok(ci);
}
Result<std::uint64_t> CudaPinnedDomain::preferred_alignment() const { return ok(std::uint64_t{256}); }
Result<std::uint64_t> CudaPinnedDomain::default_alignment() const { return ok(std::uint64_t{64}); }

} // namespace unified_buffer
#endif  // UNIFIED_BUFFER_HAS_CUDA