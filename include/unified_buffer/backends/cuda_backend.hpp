#pragma once
#include "unified_buffer/backends/domain.hpp"
#include <cstdint>

namespace unified_buffer {

// ---------------------------------------------------------------------------
// Device info
// ---------------------------------------------------------------------------
struct CudaDeviceInfo {
  int index = -1;
  std::string name;
  int compute_major = 0;
  int compute_minor = 0;
  int runtime_version = 0;
  int driver_version = 0;
  std::uint64_t total_memory = 0;
  std::uint64_t free_memory = 0;
  int multiprocessor_count = 0;
};

// Raw CUDA support helpers.  These are only available when the CUDA backend is
// compiled in (UNIFIED_BUFFER_HAS_CUDA).  They throw/return errors exactly; the
// runtime dispatches cross-domain copies through them.
namespace cuda_support {
  bool available() noexcept;
  int device_count();
  Result<CudaDeviceInfo> device_info(int index);
  Result<std::uint64_t> device_total_memory(int index);
  Result<std::uint64_t> device_free_memory(int index);
}

// CUDA device-memory domain (MemoryDomain::DEVICE).
class CudaDeviceDomain final : public IDomain {
 public:
  CudaDeviceDomain();
  ~CudaDeviceDomain() override;
  BackendId backend() const override { return BackendId::CUDA; }
  MemoryDomain domain() const override { return MemoryDomain::DEVICE; }
  const Capabilities& capabilities() const override { return caps_; }
  Result<NativeAllocation> allocate(std::uint64_t bytes, std::uint64_t alignment) override;
  Result<std::uint64_t> free(NativeAllocation&) override;
  Status zero(NativeAllocation&, std::uint64_t offset, std::uint64_t len) override;
  Status synchronize(NativeAllocation&) override;
  Status flush(NativeAllocation&) override;
  Status verify(NativeAllocation&, std::uint64_t offset, std::uint64_t len) override;
  Result<void*> map(NativeAllocation&, AccessMode) override;
  Status unmap(NativeAllocation&, void*) override;
  Status copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off,
                        const NativeAllocation& src, std::uint64_t src_off,
                        std::uint64_t len) override;
  Result<CapacityInfo> capacity() const override;
  Result<std::uint64_t> preferred_alignment() const override;
  Result<std::uint64_t> default_alignment() const override;
  int active_device() const noexcept { return active_device_; }
  void set_active_device(int d) { active_device_ = d; }
 private:
  Capabilities caps_;
  int active_device_ = 0;
  int device_count_ = 0;
};

// CUDA pinned host memory domain (MemoryDomain::PINNED_HOST).
class CudaPinnedDomain final : public IDomain {
 public:
  CudaPinnedDomain();
  ~CudaPinnedDomain() override;
  BackendId backend() const override { return BackendId::CUDA; }
  MemoryDomain domain() const override { return MemoryDomain::PINNED_HOST; }
  const Capabilities& capabilities() const override { return caps_; }
  Result<NativeAllocation> allocate(std::uint64_t bytes, std::uint64_t alignment) override;
  Result<std::uint64_t> free(NativeAllocation&) override;
  Status zero(NativeAllocation&, std::uint64_t offset, std::uint64_t len) override;
  Status synchronize(NativeAllocation&) override;
  Status flush(NativeAllocation&) override;
  Status verify(NativeAllocation&, std::uint64_t offset, std::uint64_t len) override;
  Result<void*> map(NativeAllocation&, AccessMode) override;
  Status unmap(NativeAllocation&, void*) override;
  Status copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off,
                        const NativeAllocation& src, std::uint64_t src_off,
                        std::uint64_t len) override;
  Result<CapacityInfo> capacity() const override;
  Result<std::uint64_t> preferred_alignment() const override;
  Result<std::uint64_t> default_alignment() const override;
 private:
  Capabilities caps_;
};

} // namespace unified_buffer
