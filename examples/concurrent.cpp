#include "common.hpp"
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
using namespace unified_buffer;

int main() {
  Runtime rt;
  const int kThreads = 8;
  const int kIters = 200;
  const std::uint64_t size = 4096;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&rt, size, kIters]() {
      for (int i = 0; i < kIters; ++i) {
        AllocationRequest req;
        req.size = size;
        req.domain = MemoryDomain::HOST;
        req.flags.pooled = false;       // exercise real alloc/free, no pool
        req.flags.zero_on_alloc = true;
        auto r = rt.allocate(req);
        if (!r.ok()) return;            // stop this thread if allocation fails
        BufferHandle h = std::move(r.value());
        auto m = h.map(AccessMode::READ_WRITE);
        if (m.ok()) {
          static_cast<unsigned char*>(m.value().data())[0] = static_cast<unsigned char>(i);
          m.value().release();
        }
        h.release();
      }
    });
  }
  for (auto& th : threads) th.join();

  auto st = rt.stats();
  ubex::print("[concurrent] " + std::to_string(kThreads) + " threads x " + std::to_string(kIters) +
              " alloc/free cycles: active_buffers=" + std::to_string(st.value().active_buffers) +
              " outstanding_allocations=" + std::to_string(st.value().outstanding_allocations) +
              " host_bytes=" + std::to_string(st.value().host_bytes) +
              " total_frees=" + std::to_string(st.value().total_frees));
  if (st.value().active_buffers != 0 || st.value().outstanding_allocations != 0 || st.value().host_bytes != 0) {
    ubex::print("[concurrent] FAIL: accounting did not return to zero");
    return 1;
  }
  ubex::print("[concurrent] PASS");
  return 0;
}