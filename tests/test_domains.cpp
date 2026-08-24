#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/export.hpp"
#include "test_framework.hpp"
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>

using namespace unified_buffer;

static bool has_cuda() {
  Runtime rt;
  auto d = rt.devices();
  if (!d.ok()) return false;
  for (auto& di : d.value()) if (di.available) return true;
  return false;
}

static void test_shared_export_import() {
  Runtime rt1, rt2;
  AllocationRequest req;
  req.size = 65536;
  req.domain = MemoryDomain::SHARED_HOST;
  req.flags.zero_on_alloc = false;
  req.flags.exportable = true;
  auto r1 = rt1.allocate(req);
  CHECK_TRUE(r1.ok());
  if (!r1.ok()) return;
  BufferHandle a = std::move(r1.value());
  auto m = a.map(AccessMode::READ_WRITE);
  CHECK_TRUE(m.ok());
  if (m.ok()) {
    unsigned char* p = static_cast<unsigned char*>(m.value().data());
    for (uint64_t i = 0; i < req.size; ++i) p[i] = static_cast<unsigned char>(i ^ 0x5A);
  }
  m.value().release();
  auto desc = rt1.export_buffer(a);
  CHECK_TRUE(desc.ok());
  if (!desc.ok()) return;
  auto imp = rt2.import(desc.value(), kDefaultNamespace, "imported");
  CHECK_TRUE(imp.ok());
  if (!imp.ok()) return;
  BufferHandle b = std::move(imp.value());
  CHECK_EQ((long long)b.size(), (long long)req.size);
  auto vm = b.map(AccessMode::READ);
  CHECK_TRUE(vm.ok());
  if (vm.ok()) {
    const unsigned char* p = static_cast<const unsigned char*>(vm.value().data());
    bool same = (p[0] == static_cast<unsigned char>(0 ^ 0x5A)) && (p[req.size-1] == static_cast<unsigned char>((req.size-1) ^ 0x5A));
    CHECK_TRUE(same);
  }
  vm.value().release();
  b.release();
  a.release();
}

static void test_file_reopen() {
  Runtime rt1, rt2;
  AllocationRequest req;
  req.size = 128 * 1024;
  req.domain = MemoryDomain::MMAP_STORAGE;
  req.flags.exportable = true;
  req.flags.zero_on_alloc = false;
  auto r1 = rt1.allocate(req);
  CHECK_TRUE(r1.ok());
  if (!r1.ok()) return;
  BufferHandle a = std::move(r1.value());
  auto m = a.map(AccessMode::READ_WRITE);
  CHECK_TRUE(m.ok());
  if (m.ok()) {
    unsigned char* p = static_cast<unsigned char*>(m.value().data());
    for (uint64_t i = 0; i < req.size; ++i) p[i] = static_cast<unsigned char>(i * 7 + 3);
    // flush via domain flush
  }
  m.value().release();
  CHECK_TRUE(a.verify().ok());
  auto desc = rt1.export_buffer(a);
  CHECK_TRUE(desc.ok());
  std::string path = desc.value().handle;
  a.release();  // unmap/close but file persists on disk
  // Reopen in a fresh runtime.
  ExportDescriptor d2;
  d2 = desc.value();
  auto imp = rt2.import(d2, kDefaultNamespace, "reopen");
  CHECK_TRUE(imp.ok());
  if (!imp.ok()) return;
  BufferHandle b = std::move(imp.value());
  auto vm = b.map(AccessMode::READ);
  CHECK_TRUE(vm.ok());
  if (vm.ok()) {
    const unsigned char* p = static_cast<const unsigned char*>(vm.value().data());
    bool same = true;
    for (uint64_t i = 0; i < req.size; ++i) if (p[i] != static_cast<unsigned char>(i * 7 + 3)) { same = false; break; }
    CHECK_TRUE(same);
  }
  vm.value().release();
  b.release();
  // Clean up file.
  remove(path.c_str());
}

static void test_pool_reuse_and_zeroing() {
  RuntimeConfig cfg;
  cfg.enable_pool = true;
  cfg.pool.enabled = true;
  cfg.pool.min_class_size = 4096;
  Runtime rt(cfg);
  auto s0 = rt.stats();
  AllocationRequest req;
  req.size = 16384;
  req.domain = MemoryDomain::HOST;
  req.flags.pooled = true;
  req.flags.zero_on_alloc = false;
  auto r1 = rt.allocate(req);
  CHECK_TRUE(r1.ok());
  BufferHandle a = std::move(r1.value());
  // Put a distinctive pattern so we can detect reuse.
  auto m = a.map(AccessMode::READ_WRITE); if (m.ok()) { unsigned char* p=(unsigned char*)m.value().data(); for(int i=0;i<4096;i++) p[i]=(unsigned char)0xAB; }
  m.value().release();
  a.release();  // -> pool
  auto after1 = rt.stats();
  CHECK_TRUE(after1.ok());
  CHECK_EQ((long long)after1.value().pooled_bytes, (long long)16384);
  CHECK_EQ((long long)after1.value().active_buffers, 0);
  // Now allocate a matching buffer: it should reuse the pooled allocation.
  auto r2 = rt.allocate(req);
  CHECK_TRUE(r2.ok());
  BufferHandle b = std::move(r2.value());
  auto s2 = rt.stats();
  CHECK_TRUE(s2.ok());
  CHECK_TRUE(s2.value().pool_hits >= 1);
  // Cross-owner zeroing: same namespace so NOT zeroed -> remember 0xAB pattern is visible.
  auto vm = b.map(AccessMode::READ); if (vm.ok()) { const unsigned char* p=(const unsigned char*)vm.value().data(); CHECK_EQ((long long)p[0], (long long)0xAB); }
  vm.value().release();
  b.release();
  // Cross-namespace zeroing: create ns2, allocate in ns1 release, allocate in ns2 -> zeroed.
  NamespaceConfig ns2cfg; ns2cfg.name = "ns2"; ns2cfg.zeroing = ZeroingPolicy::ON_CROSS_OWNER_REUSE;
  rt.create_namespace(ns2cfg);
  AllocationRequest req1 = req; req1.ns = kDefaultNamespace;
  auto r3 = rt.allocate(req1); CHECK_TRUE(r3.ok()); BufferHandle c = std::move(r3.value());
  auto mm = c.map(AccessMode::READ_WRITE); if (mm.ok()) { unsigned char* p=(unsigned char*)mm.value().data(); for(int i=0;i<4096;i++) p[i]=(unsigned char)0xCD; }
  mm.value().release();
  c.release();
  AllocationRequest req2 = req; req2.ns = 2;  // created namespace
  auto r4 = rt.allocate(req2); CHECK_TRUE(r4.ok()); BufferHandle d = std::move(r4.value());
  auto vm2 = d.map(AccessMode::READ); if (vm2.ok()) { const unsigned char* p=(const unsigned char*)vm2.value().data(); CHECK_EQ((long long)p[0], (long long)0); }  // zeroed
  vm2.value().release();
  d.release();
}

static void test_quota() {
  Runtime rt;
  NamespaceConfig nsc; nsc.name = "quota"; nsc.host_quota = 1ULL << 20; nsc.allocation_count_quota = 4;
  CHECK_TRUE(rt.create_namespace(nsc).ok());
  // First allocated namespace gets id 2.
  AllocationRequest req; req.ns = 2; req.size = 1ULL << 20; req.domain = MemoryDomain::HOST; req.flags.pooled = false;
  auto r1 = rt.allocate(req);
  CHECK_TRUE(r1.ok());
  auto r2 = rt.allocate(req);  // exceeds 1 MiB quota
  CHECK_FALSE(r2.ok());
  if (!r2.ok()) CHECK_EQ((long long)r2.error(), (long long)ErrorCode::quota_exceeded);
  // No leak: the failed allocation must not have reserved memory.
  auto s = rt.stats(); CHECK_TRUE(s.ok());
  CHECK_EQ((long long)s.value().active_buffers, 1);
  if (r1.ok()) r1.value().release();
  auto s2 = rt.stats(); CHECK_TRUE(s2.ok());
  CHECK_EQ((long long)s2.value().host_bytes, 0);
}

static void test_views_bounds() {
  Runtime rt;
  AllocationRequest req; req.size = 1ULL << 16; req.domain = MemoryDomain::HOST;
  auto r1 = rt.allocate(req); CHECK_TRUE(r1.ok());
  BufferHandle h = std::move(r1.value());
  auto v1 = h.view(0, 4096, AccessMode::READ); CHECK_TRUE(v1.ok());
  auto v2 = h.view(4096, 4096, AccessMode::READ_WRITE); CHECK_TRUE(v2.ok());
  // Overflow / out-of-bounds slices rejected.
  auto bad1 = h.view(65530, 4096, AccessMode::READ);
  CHECK_FALSE(bad1.ok());
  if (!bad1.ok()) CHECK_EQ((long long)bad1.error(), (long long)ErrorCode::bounds_error);
  // Zero length rejected.
  auto bad2 = h.view(0, 0, AccessMode::READ); CHECK_FALSE(bad2.ok());
  v1.value().release();
  v2.value().release();
  // Parent cannot be finalized while a view holds it.
  auto v3 = h.view(0, 16, AccessMode::READ); CHECK_TRUE(v3.ok());
  h.release();
  // v3 keeps parent alive; stats should still show an active buffer.
  auto s = rt.stats(); CHECK_TRUE(s.ok());
  CHECK_EQ((long long)s.value().active_buffers, 1);
  v3.value().release();
  auto s2 = rt.stats(); CHECK_TRUE(s2.ok());
  CHECK_EQ((long long)s2.value().active_buffers, 0);
}

static void test_leases() {
  Runtime rt;
  AllocationRequest req; req.size = 4096; req.domain = MemoryDomain::HOST;
  auto r1 = rt.allocate(req); CHECK_TRUE(r1.ok());
  BufferHandle h = std::move(r1.value());
  auto l1 = h.acquire_read(); CHECK_TRUE(l1.ok());
  auto l2 = h.acquire_read(); CHECK_TRUE(l2.ok());
  // A write lease conflicts with the active read lease.
  auto wl = h.acquire_write();
  CHECK_FALSE(wl.ok());
  if (!wl.ok()) CHECK_EQ((long long)wl.error(), (long long)ErrorCode::lease_conflict);
  l1.value().release();
  l2.value().release();
  auto wl2 = h.acquire_write(); CHECK_TRUE(wl2.ok());
  auto ex = h.acquire_exclusive();
  CHECK_FALSE(ex.ok());  // conflicts with write lease
  wl2.value().release();
  auto ex2 = h.acquire_exclusive(); CHECK_TRUE(ex2.ok());
  ex2.value().release();
  h.release();
}

static void test_migration_and_copy() {
  Runtime rt;
  bool cuda = has_cuda();
  AllocationRequest req; req.size = 65536; req.domain = MemoryDomain::HOST; req.flags.zero_on_alloc = false;
  auto r1 = rt.allocate(req); CHECK_TRUE(r1.ok());
  BufferHandle h = std::move(r1.value());
  // Fill source.
  std::vector<unsigned char> src(req.size);
  for (uint64_t i = 0; i < req.size; ++i) src[i] = (unsigned char)(i * 13 + 5);
  // Write into the buffer.
  void* hp = const_cast<void*>(h.host_data());
  std::memcpy(hp, src.data(), req.size);
  std::uint64_t gen_before = h.generation();
  MemoryDomain target = cuda ? MemoryDomain::DEVICE : MemoryDomain::SHARED_HOST;
  // Migrate.
  auto m = rt.migrate(h, target);
  CHECK_TRUE(m.ok());
  if (m.ok()) {
    BufferHandle h2 = std::move(m.value());
    CHECK_TRUE(h2.generation() > gen_before);
    // Old handle is now stale.
    auto vs = h.verify();
    CHECK_FALSE(vs.ok());
    // Read back the migrated content.
    std::vector<unsigned char> out(req.size);
    CHECK_TRUE(h2.copy_to(out.data(), 0, req.size).ok());
    bool same = (out == src);
    CHECK_TRUE(same);
    // Migrate back to host.
    auto m2 = rt.migrate(h2, MemoryDomain::HOST);
    CHECK_TRUE(m2.ok());
    if (m2.ok()) {
      BufferHandle h3 = std::move(m2.value());
      CHECK_TRUE(h3.copy_to(out.data(), 0, req.size).ok());
      CHECK_TRUE(out == src);
      CHECK_TRUE(h3.verify().ok());
      h3.release();
    }
    h2.release();
  }
  h.release();
}

int main() {
  test_shared_export_import();
  test_file_reopen();
  test_pool_reuse_and_zeroing();
  test_quota();
  test_views_bounds();
  test_leases();
  test_migration_and_copy();
  return ubtest::report("domains");
}