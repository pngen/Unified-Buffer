#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <functional>

namespace unified_buffer {

enum class MemoryDomain : uint8_t {
  HOST,
  PINNED_HOST,
  DEVICE,
  SHARED_HOST,
  MMAP_STORAGE,
};

inline std::string_view to_string(MemoryDomain d) noexcept {
  switch (d) {
    case MemoryDomain::HOST: return "HOST";
    case MemoryDomain::PINNED_HOST: return "PINNED_HOST";
    case MemoryDomain::DEVICE: return "DEVICE";
    case MemoryDomain::SHARED_HOST: return "SHARED_HOST";
    case MemoryDomain::MMAP_STORAGE: return "MMAP_STORAGE";
  }
  return "UNKNOWN";
}

enum class BackendId : uint8_t {
  HOST_MALLOC,
  CUDA,
  WIN_SHARED_FILE,
  FILE_BACKED,
};

inline std::string_view to_string(BackendId b) noexcept {
  switch (b) {
    case BackendId::HOST_MALLOC: return "HOST_MALLOC";
    case BackendId::CUDA: return "CUDA";
    case BackendId::WIN_SHARED_FILE: return "WIN_SHARED_FILE";
    case BackendId::FILE_BACKED: return "FILE_BACKED";
  }
  return "UNKNOWN";
}

struct DeviceId {
  BackendId backend = BackendId::HOST_MALLOC;
  int index = -1;

  bool operator==(const DeviceId& o) const noexcept { return backend == o.backend && index == o.index; }
  bool operator!=(const DeviceId& o) const noexcept { return !(*this == o); }
};

enum class AccessMode : uint8_t { READ, WRITE, READ_WRITE };
inline std::string_view to_string(AccessMode m) noexcept {
  switch (m) {
    case AccessMode::READ: return "READ";
    case AccessMode::WRITE: return "WRITE";
    case AccessMode::READ_WRITE: return "READ_WRITE";
  }
  return "UNKNOWN";
}

// Lifecycle state machine.  Only states actually used by the implementation
// are defined.  Legal transitions are encoded in lifecycle.cpp.
enum class BufferState : uint8_t {
  DECLARED,
  RESERVED,
  ALLOCATED,
  MAPPED,
  IN_USE,
  EXPORTING,
  EXPORTED,
  IMPORTING,
  IMPORTED,
  COPYING,
  MIGRATING,
  RELEASING,
  RELEASED,
  FAILED,
  QUARANTINED,
  INVALID,
};

inline std::string_view to_string(BufferState s) noexcept {
  switch (s) {
    case BufferState::DECLARED: return "DECLARED";
    case BufferState::RESERVED: return "RESERVED";
    case BufferState::ALLOCATED: return "ALLOCATED";
    case BufferState::MAPPED: return "MAPPED";
    case BufferState::IN_USE: return "IN_USE";
    case BufferState::EXPORTING: return "EXPORTING";
    case BufferState::EXPORTED: return "EXPORTED";
    case BufferState::IMPORTING: return "IMPORTING";
    case BufferState::IMPORTED: return "IMPORTED";
    case BufferState::COPYING: return "COPYING";
    case BufferState::MIGRATING: return "MIGRATING";
    case BufferState::RELEASING: return "RELEASING";
    case BufferState::RELEASED: return "RELEASED";
    case BufferState::FAILED: return "FAILED";
    case BufferState::QUARANTINED: return "QUARANTINED";
    case BufferState::INVALID: return "INVALID";
  }
  return "UNKNOWN";
}

// Ownership semantics of a buffer handle / backing.
enum class Ownership : uint8_t {
  RUNTIME,     // owned by the runtime: the runtime releases it
  BORROWED,    // external memory: never freed
  ADOPTED,     // external memory: runtime now owns deallocation
  IMPORTED,    // imported descriptor: runtime owns release of the import
  SHARED,      // shared ownership (e.g. external handle wrapper)
};
inline std::string_view to_string(Ownership o) noexcept {
  switch (o) {
    case Ownership::RUNTIME: return "RUNTIME";
    case Ownership::BORROWED: return "BORROWED";
    case Ownership::ADOPTED: return "ADOPTED";
    case Ownership::IMPORTED: return "IMPORTED";
    case Ownership::SHARED: return "SHARED";
  }
  return "UNKNOWN";
}

// Allocation request flags.
struct AllocationFlags {
  bool zero_on_alloc = true;      // zero memory after allocation
  bool pooled = true;             // allow pool reuse
  bool exportable = false;        // may be exported / shared
  uint8_t numa_hint = 0;          // 0=default,1=local,2=preferred,3=interleave
};

// The policy gates zeroing behaviour.
enum class ZeroingPolicy : uint8_t {
  NEVER,
  ON_ALLOCATE,
  ON_RELEASE,
  ON_CROSS_OWNER_REUSE,
  ALWAYS,
};
inline std::string_view to_string(ZeroingPolicy p) noexcept {
  switch (p) {
    case ZeroingPolicy::NEVER: return "NEVER";
    case ZeroingPolicy::ON_ALLOCATE: return "ON_ALLOCATE";
    case ZeroingPolicy::ON_RELEASE: return "ON_RELEASE";
    case ZeroingPolicy::ON_CROSS_OWNER_REUSE: return "ON_CROSS_OWNER_REUSE";
    case ZeroingPolicy::ALWAYS: return "ALWAYS";
  }
  return "UNKNOWN";
}

};  // namespace unified_buffer
