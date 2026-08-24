#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/export.hpp"
#include "test_framework.hpp"
#include <cstring>
#include <vector>
#include <string>
#include <climits>

using namespace unified_buffer;

static void test_double_free_noop() {
  Runtime rt;
  AllocationRequest req; req.size = 4096; req.domain = MemoryDomain::HOST;
  auto r = rt.allocate(req); CHECK_TRUE(r.ok());
  BufferHandle h = std::move(r.value());
  CHECK_TRUE(h.release().ok());
  // Second release is a no-op (already invalid).
  CHECK_TRUE(h.release().ok());
  CHECK_FALSE(h.valid());
  // A moved-from handle is invalid.
  BufferHandle h2; h2 = std::move(h);
  CHECK_FALSE(h2.valid());
}

static void test_invalid_requests() {
  Runtime rt;
  AllocationRequest req; req.size = 0; req.domain = MemoryDomain::HOST;
  auto rz = rt.allocate(req);
  CHECK_FALSE(rz.ok());
  // Non power-of-two alignment.
  AllocationRequest ra; ra.size = 4096; ra.domain = MemoryDomain::HOST; ra.alignment = 3;
  auto ralign = rt.allocate(ra);
  CHECK_FALSE(ralign.ok());
  if (!ralign.ok()) CHECK_EQ((long long)ralign.error(), (long long)ErrorCode::alignment_error);
  // Overflowing size (size + alignment overflow handled).
  AllocationRequest rbig; rbig.size = 1ULL << 62; rbig.domain = MemoryDomain::HOST;
  auto rbigr = rt.allocate(rbig);
  CHECK_FALSE(rbigr.ok());
}

static void test_forged_descriptor() {
  Runtime rt;
  ExportDescriptor d;
  d.format_version = 999;
  d.size = 4096;
  d.domain = MemoryDomain::HOST;
  auto imp = rt.import(d);
  CHECK_FALSE(imp.ok());
  if (!imp.ok()) CHECK_EQ((long long)imp.error(), (long long)ErrorCode::import_failure);
  // Oversized descriptor.
  ExportDescriptor d2; d2.format_version = kExportFormatVersion; d2.size = 1ULL << 60; d2.domain = MemoryDomain::HOST;
  auto imp2 = rt.import(d2);
  CHECK_FALSE(imp2.ok());
}

static void test_path_traversal_rejected() {
  Runtime rt;
  // A file import with a traversal path must be rejected.
  ExportDescriptor d;
  d.format_version = kExportFormatVersion;
  d.size = 4096;
  d.domain = MemoryDomain::MMAP_STORAGE;
  d.handle_kind = "file";
  d.handle = "..\\..\\etc\\passwd";
  auto imp = rt.import(d);
  CHECK_FALSE(imp.ok());
  if (!imp.ok()) CHECK_TRUE(imp.error() == ErrorCode::import_failure || imp.error() == ErrorCode::permission_failure);
}

static void test_cross_namespace_import_rejected() {
  Runtime rt;
  NamespaceConfig nsc; nsc.name = "other"; rt.create_namespace(nsc);  // id 2
  // Create a shared exported buffer in default ns.
  Runtime rt2;
  AllocationRequest req; req.size = 4096; req.domain = MemoryDomain::SHARED_HOST; req.flags.exportable = true;
  auto r = rt2.allocate(req); CHECK_TRUE(r.ok());
  BufferHandle h = std::move(r.value());
  auto d = rt2.export_buffer(h); CHECK_TRUE(d.ok());
  // Importing into a different namespace than exported must be rejected.
  auto imp = rt.import(d.value(), 2);
  CHECK_FALSE(imp.ok());
  if (!imp.ok()) CHECK_TRUE(imp.error() == ErrorCode::permission_failure || imp.error() == ErrorCode::import_failure);
  h.release();
}

static void test_write_readonly_denied() {
  Runtime rt;
  AllocationRequest req; req.size = 4096; req.domain = MemoryDomain::HOST; req.access = AccessMode::READ;
  auto r = rt.allocate(req); CHECK_TRUE(r.ok());
  BufferHandle h = std::move(r.value());
  auto wm = h.map(AccessMode::WRITE);
  CHECK_FALSE(wm.ok());
  auto bad_cf = h.copy_from(static_cast<const void*>("abc"), 0, 3);
  CHECK_FALSE(bad_cf.ok());
  h.release();
}

static void test_bounds_and_overflow() {
  Runtime rt;
  AllocationRequest req; req.size = 1ULL << 16; req.domain = MemoryDomain::HOST;
  auto r = rt.allocate(req); CHECK_TRUE(r.ok());
  BufferHandle h = std::move(r.value());
  // copy_to with overflow length.
  unsigned char sink[4];
  auto c = h.copy_to(sink, 0, (1ULL << 60));
  CHECK_FALSE(c.ok());
  // Negative-like offset cannot be passed since uint64; a huge offset is rejected.
  auto v = h.view(1ULL << 62, 16, AccessMode::READ);
  CHECK_FALSE(v.ok());
  h.release();
}

int main() {
  test_double_free_noop();
  test_invalid_requests();
  test_forged_descriptor();
  test_path_traversal_rejected();
  test_cross_namespace_import_rejected();
  test_write_readonly_denied();
  test_bounds_and_overflow();
  return ubtest::report("adversarial");
}
