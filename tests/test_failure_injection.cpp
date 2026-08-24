#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "test_framework.hpp"
#include <cstring>
#include <vector>

using namespace unified_buffer;

static void test_reservation_rollback_on_capacity() {
  RuntimeConfig cfg;
  cfg.host_cap = 1ULL << 20;   // 1 MiB hard cap
  cfg.enable_pool = false;
  Runtime rt(cfg);
  AllocationRequest req; req.size = 1ULL << 20; req.domain = MemoryDomain::HOST; req.flags.pooled = false;
  auto r1 = rt.allocate(req); CHECK_TRUE(r1.ok());
  // Second 1 MiB exceeds the 2 MiB cap -> out_of_capacity with rollback.
  auto r2 = rt.allocate(req);
  CHECK_FALSE(r2.ok());
  if (!r2.ok()) CHECK_TRUE(r2.error() == ErrorCode::out_of_capacity);
  // No committed memory leaked: exactly 1 MiB committed.
  auto s = rt.stats(); CHECK_TRUE(s.ok());
  if (s.ok()) CHECK_EQ((long long)s.value().host_bytes, (long long)(1 << 20));
  r1.value().release();
  auto s2 = rt.stats(); CHECK_TRUE(s2.ok());
  if (s2.ok()) CHECK_EQ((long long)s2.value().host_bytes, 0);
}

static void test_migration_failure_preserves_source() {
  RuntimeConfig cfg;
  cfg.shared_cap = 4096;   // shared memory capped tiny so migration target can't fit
  cfg.host_cap = 4ULL << 20;
  cfg.enable_pool = false;
  Runtime rt(cfg);
  AllocationRequest req; req.size = 1ULL << 20; req.domain = MemoryDomain::HOST; req.flags.pooled = false; req.flags.zero_on_alloc = false;
  auto r = rt.allocate(req); CHECK_TRUE(r.ok());
  BufferHandle h = std::move(r.value());
  std::vector<unsigned char> src(req.size); for (uint64_t i=0;i<req.size;i++) src[i]=(unsigned char)(i*11+2);
  std::memcpy(const_cast<void*>(h.host_data()), src.data(), req.size);
  // Migrating to SHARED_HOST (cap too small) must FAIL and leave the source authoritative.
  auto m = rt.migrate(h, MemoryDomain::SHARED_HOST);
  CHECK_FALSE(m.ok());
  if (!m.ok()) CHECK_TRUE(m.error() == ErrorCode::out_of_capacity);
  // Source still valid and intact.
  auto vs = h.verify();
  CHECK_TRUE(vs.ok());
  std::vector<unsigned char> out(req.size);
  CHECK_TRUE(h.copy_to(out.data(), 0, req.size).ok());
  CHECK_TRUE(out == src);
  h.release();
}

static void test_lifecycle_after_release() {
  Runtime rt;
  AllocationRequest req; req.size = 4096; req.domain = MemoryDomain::HOST;
  auto r = rt.allocate(req); CHECK_TRUE(r.ok());
  BufferHandle h = std::move(r.value());
  // An illegal transition: releasing then viewing must be rejected.
  h.release();
  auto v = h.view(0, 16, AccessMode::READ);
  CHECK_FALSE(v.ok());
  auto m = h.map(AccessMode::READ);
  CHECK_FALSE(m.ok());
  auto l = h.acquire_read();
  CHECK_FALSE(l.ok());
  // Migrating a released buffer must fail.
  auto mig = rt.migrate(h, MemoryDomain::SHARED_HOST);
  CHECK_FALSE(mig.ok());
}

int main() {
  test_reservation_rollback_on_capacity();
  test_migration_failure_preserves_source();
  test_lifecycle_after_release();
  return ubtest::report("failure-injection");
}