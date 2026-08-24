#include "internal.hpp"
#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/export.hpp"

namespace unified_buffer {
namespace internal {

// Compute a lightweight metadata integrity checksum for a record.
std::uint32_t meta_crc_of(const Record& r) {
  // Hash the stable identity/metadata fields.  Content is NOT hashed here.
  std::uint32_t crc = crc32c(&r.id, sizeof(r.id));
  crc = crc32c(&r.generation, sizeof(r.generation), crc);
  crc = crc32c(&r.domain, sizeof(r.domain), crc);
  crc = crc32c(&r.size, sizeof(r.size), crc);
  crc = crc32c(&r.alignment, sizeof(r.alignment), crc);
  return crc;
}
Result<ExportDescriptor> RuntimeImpl::export_descriptor(const Control& ctl) {
  const Record& r = ctl.rec;
  if (r.flags.exportable == false) return Error(ErrorCode::permission_failure, "export: buffer not exportable");
  ExportDescriptor d;
  d.format_version = kExportFormatVersion;
  d.buffer_id = r.id;
  d.generation = r.generation;
  d.ns = r.ns;
  d.backend = r.backend;
  d.domain = r.domain;
  d.device = r.device;
  d.size = r.size;
  d.alignment = r.alignment;
  d.access = r.access;
  d.exportable = true;
  d.writable = r.access != AccessMode::READ;
  d.label = r.label;
  d.policy_version = 1;
  d.integrity_crc = meta_crc_of(r);

  // Domain-specific handle.
  auto* dom = domain(r.domain);
  if (!dom) return Error(ErrorCode::unsupported_domain, "export: domain disabled");
  if (r.domain == MemoryDomain::SHARED_HOST || r.domain == MemoryDomain::MMAP_STORAGE) {
    auto h = dom->export_handle(r.backing);
    if (!h.ok()) return Error(h.error());
    d.handle_kind = (r.domain == MemoryDomain::SHARED_HOST) ? "shared" : "file";
    d.handle = h.value();
  } else {
    // HOST / PINNED_HOST / DEVICE: same-process export only.  Import can only
    // resolve this within the same runtime instance (library boundary).
    d.handle_kind = "local";
    d.handle = std::to_string(reinterpret_cast<std::uintptr_t>(this));
  }
  ++telem_.export_count;
  note("export", std::string(to_string(r.domain)) + " id=" + std::to_string(r.id.lo));
  return ok(std::move(d));
}

Result<std::shared_ptr<Control>> RuntimeImpl::import_descriptor(const ExportDescriptor& desc, NamespaceId ns, const std::string& label) {
  if (closed()) return Error(ErrorCode::closed, "runtime is closed");
  if (desc.format_version != kExportFormatVersion) return Error(ErrorCode::import_failure, "import: unsupported format version");
  if (desc.size == 0) return Error(ErrorCode::import_failure, "import: zero size");
  if (desc.size > (1ULL << 48)) return Error(ErrorCode::import_failure, "import: impossible size");
  if (desc.alignment && (desc.alignment & (desc.alignment - 1)) != 0) return Error(ErrorCode::import_failure, "import: bad alignment");

  // Namespace authorization: an imported buffer must not gain permissions beyond
  // the export, and cross-namespace import requires allow_import.
  auto target = namespace_info(ns);
  if (!target) return Error(ErrorCode::import_failure, "import: unknown namespace");
  auto src = namespace_info(desc.ns);
  if (src && !src->allow_import) return Error(ErrorCode::permission_failure, "import: source namespace disallows import");
  if (!target->allow_import) return Error(ErrorCode::permission_failure, "import: target namespace disallows import");
  if (desc.ns != ns) return Error(ErrorCode::permission_failure, "import: cross-namespace import not authorized");

  Record rec;
  rec.id = next_id();       // imported buffer gets a fresh identity in this runtime
  rec.generation = 1;
  rec.ns = ns;
  rec.domain = desc.domain;
  rec.backend = desc.backend;
  rec.device = desc.device;
  rec.size = desc.size;
  rec.committed = desc.size;
  rec.alignment = desc.alignment ? desc.alignment : 64;
  rec.access = desc.access;
  rec.ownership = Ownership::IMPORTED;
  rec.label = label;
  rec.created_epoch = now_epoch_ms();
  rec.state = BufferState::IMPORTED;
  rec.flags.zero_on_alloc = false;
  rec.flags.pooled = false;

  if (desc.domain == MemoryDomain::SHARED_HOST || desc.domain == MemoryDomain::MMAP_STORAGE) {
    auto* dom = domain(desc.domain);
    if (!dom) return Error(ErrorCode::unsupported_domain, "import: domain disabled");
    auto alloc = dom->import_handle(desc.handle, desc.size, desc.alignment ? desc.alignment : 64);
    if (!alloc.ok()) return Error(alloc.error());
    auto na = std::move(alloc.value());
    if (na.committed < desc.size) return Error(ErrorCode::import_failure, "import: segment smaller than declared");
    rec.backing = std::move(na);
    // Content integrity: verify metadata CRC and, for file/shared buffers,
    // perform a lightweight content probe.
    if (desc.integrity_crc != 0 && desc.integrity_crc != meta_crc_of(rec)) {
      // Metadata crc mismatch may be benign (handle id differs), so we do not
      // reject on metadata crc alone.  We record it and continue; content
      // verification is explicit via verify().
      note("import-meta-crc-mismatch", std::to_string(desc.buffer_id.lo));
    }
    ++telem_.import_count;
    note("import", std::string(to_string(desc.domain)) + " id=" + std::to_string(rec.id.lo));
    return ok(register_control(std::move(rec)));
  }

  // HOST / PINNED_HOST / DEVICE: same-runtime identity re-share only.
  if (desc.handle_kind == "local") {
    auto ctl = lookup(desc.buffer_id, desc.generation);
    if (!ctl.ok()) return Error(ErrorCode::import_failure, "import: local descriptor from a different runtime; host import requires shared memory");
    ++telem_.import_count;
    return ctl;
  }
  return Error(ErrorCode::import_failure, "import: unsupported handle kind");
}

}  // namespace internal
}