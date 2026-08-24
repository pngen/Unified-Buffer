#pragma once
#include "unified_buffer/core/identity.hpp"
#include "unified_buffer/core/result.hpp"
#include "unified_buffer/core/types.hpp"
#include <cstdint>
#include <string>

namespace unified_buffer {

// Versioned export descriptor.  Carries only what is needed to re-import a
// buffer across a library / process / device API boundary.  Never serializes
// raw process pointers as portable handles.
constexpr std::uint32_t kExportFormatVersion = 1;

struct ExportDescriptor {
  std::uint32_t format_version = kExportFormatVersion;
  BufferId buffer_id;
  BufferGeneration generation = 0;
  NamespaceId ns = kDefaultNamespace;
  BackendId backend = BackendId::HOST_MALLOC;
  MemoryDomain domain = MemoryDomain::HOST;
  DeviceId device;
  std::uint64_t size = 0;
  std::uint64_t alignment = 0;
  AccessMode access = AccessMode::READ_WRITE;
  // Handle type + opaque handle metadata (e.g. a named/shared-memory name or a
  // file path).  Contents depend on `domain` / `backend`.
  std::string handle_kind;   // "shared", "file", "host", "device", "ipc"
  std::string handle;        // opaque, backend-specific handle data
  std::uint32_t integrity_crc = 0;
  bool exportable = false;
  bool writable = true;
  std::uint32_t policy_version = 1;
  // Human-readable provenance, not trusted at import.
  std::string label;
};

} // namespace unified_buffer
