#include "unified_buffer/runtime.hpp"
#include "test_framework.hpp"
#include <cstring>
#include <vector>

using namespace unified_buffer;

static void test_host_roundtrip() {
  Runtime rt;
  AllocationRequest req;
  req.size = (1 << 20); // 1 MiB
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = true;
  req.flags.pooled = false;
  auto r = rt.allocate(req);
  CHECK_TRUE(r.ok());
  if (!r.ok()) return;
  BufferHandle h = std::move(r.value());
  CHECK_TRUE(h.valid());
  CHECK_EQ((long long)h.size(), (long long)(1 << 20));
  CHECK_TRUE(h.domain() == MemoryDomain::HOST);

  // Map and write a pattern.
  auto m = h.map(AccessMode::READ_WRITE);
  CHECK_TRUE(m.ok());
  if (m.ok()) {
    unsigned char* p = static_cast<unsigned char*>(m.value().data());
    for (uint64_t i = 0; i < (1 << 20); ++i) p[i] = static_cast<unsigned char>(i * 31 + 7);
  }
  m.value().release();

  // Read back through a view.
  auto v = h.view(0, (1 << 20), AccessMode::READ);
  CHECK_TRUE(v.ok());
  if (v.ok()) {
    const unsigned char* p = static_cast<const unsigned char*>(v.value().data());
    bool good = true;
    for (uint64_t i = 0; i < (1 << 20); ++i) if (p[i] != static_cast<unsigned char>(i * 31 + 7)) { good = false; break; }
    CHECK_TRUE(good);
  }
  v.value().release();

  // Checksum is stable.
  auto c1 = h.checksum();
  auto c2 = h.checksum();
  CHECK_TRUE(c1.ok() && c2.ok());
  if (c1.ok() && c2.ok()) CHECK_EQ((long long)c1.value(), (long long)c2.value());

  // Verify integrity.
  CHECK_TRUE(h.verify().ok());

  // Release and confirm accounting back to baseline.
  CHECK_TRUE(h.release().ok());
  CHECK_FALSE(h.valid());
}

static void test_raii_and_baseline() {
  Runtime rt;
  auto before = rt.stats();
  CHECK_TRUE(before.ok());
  {
    AllocationRequest req;
    req.size = 4096;
    req.domain = MemoryDomain::HOST;
    req.flags.pooled = false;
    auto h = rt.allocate(req);
    CHECK_TRUE(h.ok());
  }  // handle scope ends -> buffer released
  auto after = rt.stats();
  CHECK_TRUE(after.ok());
  CHECK_EQ((long long)after.value().active_buffers, 0);
  CHECK_EQ((long long)after.value().outstanding_allocations, 0);
  CHECK_EQ((long long)after.value().host_bytes, 0);
  CHECK_EQ((long long)after.value().total_frees, 1);
}

int main() {
  test_host_roundtrip();
  test_raii_and_baseline();
  return ubtest::report("smoke");
}