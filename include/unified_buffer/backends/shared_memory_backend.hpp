#pragma once
#include "unified_buffer/backends/domain.hpp"
#include <cstdint>

namespace unified_buffer {

// Interprocess shared-memory backend (MemoryDomain::SHARED_HOST).
// Uses OS file-mapping objects on Windows, /dev/shm on Linux.
class SharedMemoryDomain final : public IDomain {
 public:
  explicit SharedMemoryDomain(std::uint64_t cap = 0);
  ~SharedMemoryDomain() override = default;
  BackendId backend() const override { return BackendId::WIN_SHARED_FILE; }
  MemoryDomain domain() const override { return MemoryDomain::SHARED_HOST; }
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
  Result<std::string> export_handle(const NativeAllocation&) const override;
  Result<NativeAllocation> import_handle(const std::string&, std::uint64_t size, std::uint64_t alignment) const override;
  void set_capacity(std::uint64_t cap) { cap_ = cap; }
 private:
  void init();
  std::uint64_t cap_ = 0;
  Capabilities caps_;
};

} // namespace unified_buffer
