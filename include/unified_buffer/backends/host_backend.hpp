#pragma once
#include "unified_buffer/backends/domain.hpp"
#include <cstdint>

namespace unified_buffer {

// Standard aligned host-memory backend (CPU-accessible, pageable).
class HostDomain final : public IDomain {
 public:
  explicit HostDomain(std::uint64_t capacity_hint = 0);
  ~HostDomain() override = default;

  BackendId backend() const override { return BackendId::HOST_MALLOC; }
  MemoryDomain domain() const override { return MemoryDomain::HOST; }
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

  void set_capacity_hint(std::uint64_t bytes) { cap_hint_ = bytes; }

 private:
  void init_capabilities();
  std::uint64_t cap_hint_ = 0;
  std::uint64_t total_bytes_ = 0;
  Capabilities caps_;
};

} // namespace unified_buffer
