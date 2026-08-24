#pragma once
#include "unified_buffer/core/identity.hpp"
#include "unified_buffer/core/result.hpp"
#include "unified_buffer/core/types.hpp"
#include <cstdint>
#include <memory>

namespace unified_buffer {

namespace internal { class RuntimeImpl; struct Control; }

// Lease kind.
enum class LeaseKind : std::uint8_t { READ, WRITE, EXCLUSIVE };

// --- Scoped lease (pin) granting access to a buffer ---
class BufferLease {
 public:
  BufferLease() = default;
  ~BufferLease();
  BufferLease(BufferLease&&) noexcept;
  BufferLease& operator=(BufferLease&&) noexcept;
  BufferLease(const BufferLease&) = delete;
  BufferLease& operator=(const BufferLease&) = delete;
  void release();
  bool valid() const { return ctl_ != nullptr; }
  BufferId id() const;
  BufferGeneration generation() const;
  LeaseKind kind() const { return kind_; }
 private:
  friend class BufferHandle;
  BufferLease(std::shared_ptr<internal::RuntimeImpl> rt, std::shared_ptr<internal::Control> ctl,
              BufferGeneration gen, LeaseKind kind);
  std::shared_ptr<internal::RuntimeImpl> rt_;
  std::shared_ptr<internal::Control> ctl_;
  BufferGeneration gen_ = 0;
  LeaseKind kind_ = LeaseKind::READ;
};

// --- Scoped CPU mapping of a buffer ---
class BufferMap {
 public:
  BufferMap() = default;
  ~BufferMap();
  BufferMap(BufferMap&&) noexcept;
  BufferMap& operator=(BufferMap&&) noexcept;
  BufferMap(const BufferMap&) = delete;
  BufferMap& operator=(const BufferMap&) = delete;
  void release();
  bool valid() const { return ctl_ != nullptr; }
  void* data() const { return data_; }
  BufferId id() const;
  BufferGeneration generation() const;
  AccessMode access() const { return access_; }
 private:
  friend class BufferHandle;
  BufferMap(std::shared_ptr<internal::RuntimeImpl> rt, std::shared_ptr<internal::Control> ctl,
            BufferGeneration gen, AccessMode access, void* data);
  std::shared_ptr<internal::RuntimeImpl> rt_;
  std::shared_ptr<internal::Control> ctl_;
  BufferGeneration gen_ = 0;
  AccessMode access_ = AccessMode::READ;
  void* data_ = nullptr;
};

// --- Non-owning slice/view of a parent buffer ---
class BufferView {
 public:
  BufferView() = default;
  ~BufferView();
  BufferView(BufferView&&) noexcept;
  BufferView& operator=(BufferView&&) noexcept;
  BufferView(const BufferView&) = delete;
  BufferView& operator=(const BufferView&) = delete;
  bool valid() const { return ctl_ != nullptr; }
  BufferId parent_id() const;
  BufferGeneration parent_generation() const;
  std::uint64_t offset() const { return off_; }
  std::uint64_t length() const { return len_; }
  std::uint64_t size() const { return len_; }
  AccessMode access() const { return access_; }
  const void* data() const { return data_; }
  void release();
 private:
  friend class BufferHandle;
  BufferView(std::shared_ptr<internal::RuntimeImpl> rt, std::shared_ptr<internal::Control> ctl,
             BufferGeneration gen, std::uint64_t off, std::uint64_t len, AccessMode access, void* data);
  std::shared_ptr<internal::RuntimeImpl> rt_;
  std::shared_ptr<internal::Control> ctl_;
  BufferGeneration gen_ = 0;
  std::uint64_t off_ = 0;
  std::uint64_t len_ = 0;
  AccessMode access_ = AccessMode::READ;
  void* data_ = nullptr;
};

// --- The primary logical buffer handle (generation-aware, RAII, move-only) ---
class BufferHandle {
 public:
  BufferHandle() = default;
  ~BufferHandle();
  BufferHandle(BufferHandle&&) noexcept;
  BufferHandle& operator=(BufferHandle&&) noexcept;
  BufferHandle(const BufferHandle&) = delete;
  BufferHandle& operator=(const BufferHandle&) = delete;

  BufferId id() const { return id_; }
  BufferGeneration generation() const { return gen_; }
  MemoryDomain domain() const;
  std::uint64_t size() const;
  std::uint64_t aligned_size() const;
  std::uint64_t alignment() const;
  NamespaceId namespace_id() const;
  BufferState state() const;
  bool valid() const { return valid_; }
  explicit operator bool() const { return valid_; }

  Status release();
  Result<BufferHandle> share() const;
  Result<BufferView> view(std::uint64_t offset, std::uint64_t len, AccessMode access);
  Result<BufferMap> map(AccessMode access);
  Result<BufferLease> acquire_read();
  Result<BufferLease> acquire_write();
  Result<BufferLease> acquire_exclusive();
  Status verify();
  Result<std::uint32_t> checksum();
  Status copy_to(void* dst, std::uint64_t dst_offset, std::uint64_t len);
  Status copy_from(const void* src, std::uint64_t src_offset, std::uint64_t len);
  // Returns the raw device pointer (borrowed, tied to this handle + generation).
  Result<void*> device_pointer() const;
  const void* host_data() const;

  std::shared_ptr<internal::RuntimeImpl> runtime_impl() const { return rt_; }
  std::shared_ptr<internal::Control> control() const { return ctl_; }

 private:
  friend class Runtime;

  void reset();  // drop ref + null
  Status validate_alive() const;
  Status ensure_valid(AccessMode want) const;

  std::shared_ptr<internal::RuntimeImpl> rt_;
  std::shared_ptr<internal::Control> ctl_;
  BufferId id_;
  BufferGeneration gen_ = 0;
  bool valid_ = false;
};

} // namespace unified_buffer