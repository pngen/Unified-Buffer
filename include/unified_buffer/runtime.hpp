#pragma once
#include "unified_buffer/config.hpp"
#include "unified_buffer/core/identity.hpp"
#include "unified_buffer/core/result.hpp"
#include "unified_buffer/core/types.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/export.hpp"
#include "unified_buffer/telemetry.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace unified_buffer {

namespace internal { class RuntimeImpl; }
class BufferHandle;

// A request to allocate a logical buffer.
struct AllocationRequest {
  NamespaceId ns = kDefaultNamespace;
  MemoryDomain domain = MemoryDomain::HOST;
  DeviceId device;
  std::uint64_t size = 0;
  std::uint64_t alignment = 0;   // 0 = backend default
  AccessMode access = AccessMode::READ_WRITE;
  AllocationFlags flags;
  std::string label;
  // Optional zeroing override (else namespace policy applies).
  std::optional<ZeroingPolicy> zeroing;
};

// Describes externally allocated memory to wrap.  Ownership is explicit.
enum class ExternalOwnership : std::uint8_t { BORROW, ADOPT, SHARED };

struct ExternalMemoryDesc {
  void* pointer = nullptr;
  std::uint64_t size = 0;
  std::uint64_t alignment = 0;
  MemoryDomain domain = MemoryDomain::HOST;
  DeviceId device;
  ExternalOwnership ownership = ExternalOwnership::BORROW;
};

// The public runtime facade.  A thin, moveable handle to a shared
// implementation so that BufferHandles can outlive the facade name.
class Runtime {
 public:
  explicit Runtime(const RuntimeConfig& cfg = {});
  ~Runtime();
  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // --- creation ---
  Result<BufferHandle> allocate(const AllocationRequest& req);
  Result<BufferHandle> wrap_external(const ExternalMemoryDesc& desc);
  Result<BufferHandle> import(const ExportDescriptor& desc, NamespaceId ns = kDefaultNamespace, const std::string& label = {});

  // --- introspection ---
  Result<ExportDescriptor> export_buffer(const BufferHandle& h);
  Result<BufferHandle> migrate(const BufferHandle& h, MemoryDomain target);
  Result<std::vector<BackendInfo>> backends() const;
  Result<std::vector<DeviceInfo>> devices() const;
  Result<std::vector<DomainInfo>> domains() const;
  Result<RuntimeStats> stats() const;
  Result<std::vector<std::pair<std::string, std::string>>> decisions() const;

  // --- lifecycle ---
  Status invalidate(BufferId id);
  Status pool_trim();
  void shutdown();
  bool closed() const;

  // --- namespaces ---
  Status create_namespace(const NamespaceConfig& cfg);
  Result<NamespaceConfig> namespace_info(NamespaceId id) const;

  // Access to the shared implementation for internal/advanced use.
  std::shared_ptr<internal::RuntimeImpl> impl() const { return impl_; }

 private:
  std::shared_ptr<internal::RuntimeImpl> impl_;
};

} // namespace unified_buffer