#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "test_framework.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>

using namespace unified_buffer;

static void worker(Runtime& rt, int tid, int iters, std::atomic<long>& ok_count) {
  for (int i = 0; i < iters; ++i) {
    AllocationRequest req;
    req.size = 1024 + ((i + tid) % 8) * 4096;
    req.domain = MemoryDomain::HOST;
    auto r = rt.allocate(req);
    if (!r.ok()) continue;
    BufferHandle h = std::move(r.value());
    auto hv = h.map(AccessMode::READ_WRITE);
    if (hv.ok()) { *static_cast<volatile unsigned char*>(hv.value().data()) = (unsigned char)tid; hv.value().release(); }
    auto v = h.view(0, 16, AccessMode::READ);
    if (v.ok()) v.value().release();
    h.release();
    ++ok_count;
  }
}

static void concurrent_alloc() {
  const int nthreads = 8;
  const int iters = 2000;
  Runtime rt;
  std::vector<std::thread> threads;
  std::atomic<long> okc{0};
  for (int t = 0; t < nthreads; ++t) threads.emplace_back(worker, std::ref(rt), t, iters, std::ref(okc));
  for (auto& th : threads) th.join();
  CHECK_EQ((long long)okc.load(), (long long)(nthreads * iters));
  auto s = rt.stats();
  CHECK_TRUE(s.ok());
  if (s.ok()) {
    CHECK_EQ((long long)s.value().active_buffers, 0);
    CHECK_EQ((long long)s.value().outstanding_allocations, 0);
  }
}

static void concurrent_leases_maps() {
  const int nthreads = 8;
  Runtime rt;
  AllocationRequest req; req.size = 1 << 16; req.domain = MemoryDomain::HOST; req.flags.pooled = true;
  auto r = rt.allocate(req); CHECK_TRUE(r.ok());
  BufferHandle base = std::move(r.value());
  std::vector<std::thread> threads;
  std::atomic<int> read_success{0}, write_success{0}, conflicts{0};
  for (int t = 0; t < nthreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < 300; ++i) {
        if (t % 2 == 0) {
          auto l = base.acquire_read();
          if (l.ok()) { ++read_success; l.value().release(); }
        } else {
          auto l = base.acquire_write();
          if (l.ok()) { ++write_success; l.value().release(); }
          else if (!l.ok() && l.error() == ErrorCode::lease_conflict) ++conflicts;
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  // Reads must always succeed; writes may conflict.
  CHECK_TRUE(read_success.load() > 0);
  base.release();
}

int main() {
  concurrent_alloc();
  concurrent_leases_maps();
  return ubtest::report("concurrency");
}
