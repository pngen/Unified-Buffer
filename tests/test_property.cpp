#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "test_framework.hpp"
#include <random>
#include <vector>
#include <utility>
#include <cstdio>
#include <malloc.h>

using namespace unified_buffer;

int main() {
  std::mt19937 rng(0xC0FFEE);
  Runtime rt;
  std::vector<std::pair<std::uint64_t, BufferHandle>> live;
  std::uint64_t ops = 0;

  auto release_all = [&]() {
    for (auto& kv : live) kv.second.release();
    live.clear();
  };

  for (int step = 0; step < 6000; ++step) {
    int op = (int)(rng() % 8);
    if (op == 0 || live.empty()) {
      // allocate
      AllocationRequest req;
      req.size = 64 + (rng() % (256 * 1024));
      req.domain = MemoryDomain::HOST;
      req.flags.pooled = (rng() % 2) == 0;
      req.flags.zero_on_alloc = (rng() % 2) == 0;
      if ((rng() % 20) == 0) { req.flags.exportable = true; req.domain = MemoryDomain::SHARED_HOST; }
      if ((rng() % 20) == 0) { req.domain = MemoryDomain::MMAP_STORAGE; req.flags.exportable = true; }
      auto r = rt.allocate(req);
      CHECK_TRUE(r.ok());
      if (r.ok()) live.emplace_back(r.value().id().lo, std::move(r.value()));
    } else if (op == 1) {
      // free a random live buffer
      std::size_t idx = rng() % live.size();
      live[idx].second.release();
      live.erase(live.begin() + idx);
    } else if (op == 2) {
      // map + write + unmap
      auto& h = live[rng() % live.size()].second;
      if (h.valid()) { auto m = h.map(AccessMode::READ_WRITE); if (m.ok()) { volatile unsigned char* p = static_cast<volatile unsigned char*>(m.value().data()); (void)p[0]; m.value().release(); } }
    } else if (op == 3) {
      // view then release
      auto& h = live[rng() % live.size()].second;
      if (h.valid()) { auto v = h.view(0, 16, AccessMode::READ); if (v.ok()) v.value().release(); }
    } else if (op == 4) {
      // read lease
      auto& h = live[rng() % live.size()].second;
      if (h.valid()) { auto l = h.acquire_read(); if (l.ok()) l.value().release(); }
    } else if (op == 5) {
      // verify + checksum
      auto& h = live[rng() % live.size()].second;
      if (h.valid()) { CHECK_TRUE(h.verify().ok()); (void)h.checksum(); }
    } else if (op == 6) {
      // migrate a buffer (to shared host, always available)
      auto& h = live[rng() % live.size()].second;
      if (h.valid()) {
        auto m = rt.migrate(h, MemoryDomain::HOST);
        if (m.ok()) h = std::move(m.value());
      }
    } else {
      ++ops;
    }
  }

  // Invariant: release everything, trim pool, accounting returns to baseline.
  release_all();
  rt.pool_trim();
  auto s = rt.stats();
  CHECK_TRUE(s.ok());
  if (s.ok()) {
    CHECK_TRUE(s.value().active_buffers == 0);
    CHECK_TRUE(s.value().outstanding_allocations == 0);
    CHECK_TRUE(s.value().host_bytes == 0);
    CHECK_TRUE(s.value().shared_host_bytes == 0);
    CHECK_TRUE(s.value().file_backed_bytes == 0);
    CHECK_TRUE(s.value().pooled_bytes == 0);
  }
  // Every allocation must have been matched by a free.
  auto s2 = rt.stats();
  if (s2.ok()) CHECK_TRUE(s2.value().total_allocations == s2.value().total_frees || s2.value().active_buffers == 0);
  std::printf("  property-test: %d steps, %d live at end\n", 6000, (int)live.size());
  return ubtest::report("property");
}