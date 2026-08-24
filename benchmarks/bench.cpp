// unified_buffer benchmark suite
// Measures real latencies / throughputs for the working C++20 systems runtime.
// Emits a human-readable table and machine-readable lines of the form
//   bench,<name>,<measure>,<value>,<unit>
// Domains that are unavailable are reported and skipped (never faked).

#include "unified_buffer/runtime.hpp"
#include "unified_buffer/telemetry.hpp"
#include "unified_buffer/config.hpp"
#include "unified_buffer/core/types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef UNIFIED_BUFFER_HAS_CUDA
#include <cuda_runtime.h>
#endif

using namespace unified_buffer;
using Clock = std::chrono::high_resolution_clock;

namespace {

constexpr std::uint64_t KiB = 1024ULL;
constexpr std::uint64_t MiB = 1024ULL * 1024ULL;

// Volatile sink so the compiler cannot discard the work a measurement performs.
volatile std::uint64_t g_sink = 0;

struct Metric {
  std::string name;
  std::string measure;
  double value = 0.0;
  std::string unit;
};

std::vector<Metric> g_metrics;
std::vector<std::string> g_skipped;

double ns_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::nano>(b - a).count();
}

void report_metric(const std::string& name, const std::string& measure,
                   double value, const std::string& unit) {
  Metric m;
  m.name = name; m.measure = measure; m.value = value; m.unit = unit;
  g_metrics.push_back(m);
  std::cout << std::left << std::setw(28) << name << std::setw(20) << measure
            << std::right << std::setw(16) << std::fixed << std::setprecision(5)
            << value << " " << unit << std::endl;
  std::cout << "bench," << name << "," << measure << "," << std::fixed
            << std::setprecision(6) << value << "," << unit << std::endl;
}

void report_skip(const std::string& name, const std::string& reason) {
  g_skipped.push_back(name);
  std::cout << std::left << std::setw(28) << name << std::setw(20) << "SKIPPED"
            << std::right << std::setw(16) << "- "
            << "- " << std::endl;
  std::cout << "bench," << name << ",skipped,-,na" << std::endl;
  std::cout << "       // " << reason << std::endl;
}

bool domain_available(Runtime& rt, MemoryDomain d) {
  auto doms = rt.domains();
  if (!doms.ok()) return false;
  for (const auto& di : doms.value()) {
    if (di.domain == d && di.enabled) return true;
  }
  return false;
}

bool cuda_device_available(Runtime& rt) {
  auto devs = rt.devices();
  if (devs.ok()) {
    for (const auto& dev : devs.value()) {
      if (dev.available && dev.backend == BackendId::CUDA) return true;
    }
  }
  return domain_available(rt, MemoryDomain::DEVICE);
}

BufferHandle try_alloc(Runtime& rt, const AllocationRequest& req) {
  auto r = rt.allocate(req);
  return r.ok() ? std::move(r.value()) : BufferHandle{};
}

template <typename F>
double ns_per_op(F&& op, long long iters, long long warm) {
  for (long long i = 0; i < warm; ++i) op();
  auto t0 = Clock::now();
  for (long long i = 0; i < iters; ++i) op();
  auto t1 = Clock::now();
  return ns_between(t0, t1) / static_cast<double>(iters);
}

// Times only the allocate() call; the release happens after the clock is read.
double alloc_only_ns(Runtime& rt, const AllocationRequest& req, long long iters,
                     long long warm) {
  for (long long i = 0; i < warm; ++i) { auto h = try_alloc(rt, req); h.release(); }
  double total = 0.0;
  for (long long i = 0; i < iters; ++i) {
    auto t0 = Clock::now();
    auto h = try_alloc(rt, req);
    auto t1 = Clock::now();
    if (h.valid()) { g_sink += h.size(); } else { g_sink += 1; }
    h.release();
    total += ns_between(t0, t1);
  }
  return total / static_cast<double>(iters);
}

// Times only the release() call; allocation happens before the clock is read.
double free_only_ns(Runtime& rt, const AllocationRequest& req, long long iters,
                    long long warm) {
  for (long long i = 0; i < warm; ++i) { auto h = try_alloc(rt, req); h.release(); }
  double total = 0.0;
  for (long long i = 0; i < iters; ++i) {
    auto h = try_alloc(rt, req);
    if (!h.valid()) { g_sink += 1; continue; }
    auto t0 = Clock::now();
    h.release();
    auto t1 = Clock::now();
    total += ns_between(t0, t1);
  }
  return total / static_cast<double>(iters);
}

// Full allocate + release cycles per second.
double ops_per_sec(Runtime& rt, const AllocationRequest& req, long long iters,
                   long long warm) {
  for (long long i = 0; i < warm; ++i) { auto h = try_alloc(rt, req); h.release(); }
  auto t0 = Clock::now();
  for (long long i = 0; i < iters; ++i) {
    auto h = try_alloc(rt, req);
    if (h.valid()) { g_sink += h.size(); } else { g_sink += 1; }
    h.release();
  }
  auto t1 = Clock::now();
  double secs = ns_between(t0, t1) / 1e9;
  return static_cast<double>(iters) / secs;
}

// ---- individual benchmark functions ---------------------------------------

void bench_host_alloc_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 4 * MiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto op = [&]() {
    auto h = try_alloc(rt, req);
    if (h.valid()) { g_sink += h.size(); } else { g_sink += 1; }
    h.release();
  };
  double ns = ns_per_op(op, 2000, 100);
  report_metric("host_alloc_latency", "allocate+release", ns, "ns/op");
}

void bench_host_free_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 4 * MiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  double ns = free_only_ns(rt, req, 2000, 100);
  report_metric("host_free_latency", "release", ns, "ns/op");
}

void bench_pooled_alloc_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 8 * KiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = true;
  double ns = alloc_only_ns(rt, req, 200000, 2000);
  report_metric("pooled_alloc_latency", "allocate", ns, "ns/op");
}

void bench_pooled_free_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 8 * KiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = true;
  double ns = free_only_ns(rt, req, 200000, 2000);
  report_metric("pooled_free_latency", "release", ns, "ns/op");
}

void bench_aligned_alloc_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 1 * MiB;
  req.domain = MemoryDomain::HOST;
  req.alignment = 4096;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto op = [&]() {
    auto h = try_alloc(rt, req);
    if (h.valid()) { g_sink += h.alignment(); } else { g_sink += 1; }
    h.release();
  };
  double ns = ns_per_op(op, 5000, 200);
  report_metric("aligned_alloc_latency", "allocate+release", ns, "ns/op");
}

void bench_small_buf_throughput(Runtime& rt) {
  AllocationRequest req;
  req.size = 4 * KiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  double ops = ops_per_sec(rt, req, 300000, 2000);
  report_metric("small_buf_throughput", "allocations", ops, "ops/sec");
}

void bench_large_buf_throughput(Runtime& rt) {
  AllocationRequest req;
  req.size = 16 * MiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  double ops = ops_per_sec(rt, req, 2000, 50);
  report_metric("large_buf_throughput", "allocations", ops, "ops/sec");
}

void bench_pool_hit_rate() {
  // Fresh runtime so the pool counters start at zero.
  Runtime rt_pool;
  AllocationRequest req;
  req.size = 8 * KiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = true;
  const long long rounds = 200000;
  for (long long i = 0; i < rounds; ++i) { auto h = try_alloc(rt_pool, req); h.release(); }
  auto st = rt_pool.stats();
  if (!st.ok()) { report_skip("pool_hit_rate", "stats() failed"); return; }
  double hits = static_cast<double>(st.value().pool_hits);
  double misses = static_cast<double>(st.value().pool_misses);
  double rate = (hits + misses) > 0.0 ? hits / (hits + misses) : 0.0;
  report_metric("pool_hit_rate", "hit-fraction", rate, "fraction");
}

void bench_host_memcpy_bandwidth(Runtime& rt) {
  constexpr std::uint64_t S = 32 * MiB;
  AllocationRequest req;
  req.size = S;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto src = rt.allocate(req);
  auto dst = rt.allocate(req);
  if (!src.ok() || !dst.ok()) { report_skip("host_memcpy_bandwidth", "host alloc failed"); return; }
  auto sm = src.value().map(AccessMode::READ_WRITE);
  auto dm = dst.value().map(AccessMode::READ_WRITE);
  if (!sm.ok() || !dm.ok()) { report_skip("host_memcpy_bandwidth", "map failed"); return; }
  char* sp = static_cast<char*>(sm.value().data());
  char* dp = static_cast<char*>(dm.value().data());
  std::memset(sp, 0xA5, static_cast<std::size_t>(S));
  std::memset(dp, 0x00, static_cast<std::size_t>(S));
  const long long iters = 30;
  for (long long w = 0; w < 3; ++w) std::memcpy(dp, sp, static_cast<std::size_t>(S));
  auto t0 = Clock::now();
  for (long long i = 0; i < iters; ++i) std::memcpy(dp, sp, static_cast<std::size_t>(S));
  auto t1 = Clock::now();
  double secs = ns_between(t0, t1) / 1e9;
  double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                (1024.0 * 1024.0) / secs;
  g_sink += static_cast<std::uint64_t>(static_cast<unsigned char>(dp[S - 1]));
  const bool intact = (std::memcmp(dp, sp, 4096) == 0);
  g_sink += intact ? 1u : 0u;
  report_metric("host_memcpy_bandwidth", "memcpy", mbps, "MiB/s");
}

void bench_shared_map_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 4 * MiB;
  req.domain = MemoryDomain::SHARED_HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto op = [&]() {
    auto h = try_alloc(rt, req);
    if (!h.valid()) { g_sink += 1; return; }
    auto m = h.map(AccessMode::READ_WRITE);
    if (m.ok()) { g_sink += static_cast<std::uint64_t>(m.value().data() != nullptr); }
    // scoped release on return
  };
  double ns = ns_per_op(op, 300, 30);
  report_metric("shared_map_latency", "allocate+map", ns, "ns/op");
}

void bench_shared_copy_bandwidth(Runtime& rt) {
  constexpr std::uint64_t S = 32 * MiB;
  AllocationRequest req;
  req.size = S;
  req.domain = MemoryDomain::SHARED_HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto src = rt.allocate(req);
  auto dst = rt.allocate(req);
  if (!src.ok() || !dst.ok()) { report_skip("shared_copy_bandwidth", "shared alloc failed"); return; }
  auto sm = src.value().map(AccessMode::READ_WRITE);
  auto dm = dst.value().map(AccessMode::READ_WRITE);
  if (!sm.ok() || !dm.ok()) { report_skip("shared_copy_bandwidth", "map failed"); return; }
  char* sp = static_cast<char*>(sm.value().data());
  char* dp = static_cast<char*>(dm.value().data());
  std::memset(sp, 0xC3, static_cast<std::size_t>(S));
  std::memset(dp, 0x00, static_cast<std::size_t>(S));
  const long long iters = 20;
  for (long long w = 0; w < 3; ++w) std::memcpy(dp, sp, static_cast<std::size_t>(S));
  auto t0 = Clock::now();
  for (long long i = 0; i < iters; ++i) std::memcpy(dp, sp, static_cast<std::size_t>(S));
  auto t1 = Clock::now();
  double secs = ns_between(t0, t1) / 1e9;
  double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                (1024.0 * 1024.0) / secs;
  g_sink += static_cast<std::uint64_t>(static_cast<unsigned char>(dp[S - 1]));
  report_metric("shared_copy_bandwidth", "memcpy", mbps, "MiB/s");
}

void bench_file_map_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 1 * MiB;
  req.domain = MemoryDomain::MMAP_STORAGE;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto op = [&]() {
    auto h = try_alloc(rt, req);
    if (!h.valid()) { g_sink += 1; return; }
    auto m = h.map(AccessMode::READ_WRITE);
    if (m.ok()) { g_sink += static_cast<std::uint64_t>(m.value().data() != nullptr); }
  };
  double ns = ns_per_op(op, 20, 5);
  report_metric("file_map_latency", "allocate+map", ns, "ns/op");
}

void bench_file_write_bandwidth(Runtime& rt) {
  constexpr std::uint64_t S = 16 * MiB;
  AllocationRequest req;
  req.size = S;
  req.domain = MemoryDomain::MMAP_STORAGE;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto buf = rt.allocate(req);
  if (!buf.ok()) { report_skip("file_write_bandwidth", "file alloc failed"); return; }
  auto m = buf.value().map(AccessMode::READ_WRITE);
  if (!m.ok()) { report_skip("file_write_bandwidth", "map failed"); return; }
  char* fp = static_cast<char*>(m.value().data());
  const long long iters = 10;
  for (long long w = 0; w < 2; ++w) std::memset(fp, 0x6F, static_cast<std::size_t>(S));
  auto t0 = Clock::now();
  for (long long i = 0; i < iters; ++i) std::memset(fp, 0x6F, static_cast<std::size_t>(S));
  auto t1 = Clock::now();
  double secs = ns_between(t0, t1) / 1e9;
  double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                (1024.0 * 1024.0) / secs;
  g_sink += static_cast<std::uint64_t>(static_cast<unsigned char>(fp[S - 1]));
  report_metric("file_write_bandwidth", "write", mbps, "MiB/s");
}

void bench_file_read_bandwidth(Runtime& rt) {
  constexpr std::uint64_t S = 16 * MiB;
  AllocationRequest req;
  req.size = S;
  req.domain = MemoryDomain::MMAP_STORAGE;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto buf = rt.allocate(req);
  if (!buf.ok()) { report_skip("file_read_bandwidth", "file alloc failed"); return; }
  auto m = buf.value().map(AccessMode::READ_WRITE);
  if (!m.ok()) { report_skip("file_read_bandwidth", "map failed"); return; }
  char* fp = static_cast<char*>(m.value().data());
  std::memset(fp, 0x3A, static_cast<std::size_t>(S));

  AllocationRequest hreq;
  hreq.size = S;
  hreq.domain = MemoryDomain::HOST;
  hreq.flags.zero_on_alloc = false;
  hreq.flags.pooled = false;
  auto dst = rt.allocate(hreq);
  if (!dst.ok()) { report_skip("file_read_bandwidth", "host alloc failed"); return; }
  auto hmp = dst.value().map(AccessMode::READ_WRITE);
  if (!hmp.ok()) { report_skip("file_read_bandwidth", "host map failed"); return; }
  char* hp = static_cast<char*>(hmp.value().data());

  const long long iters = 20;
  for (long long w = 0; w < 2; ++w) std::memcpy(hp, fp, static_cast<std::size_t>(S));
  auto t0 = Clock::now();
  for (long long i = 0; i < iters; ++i) std::memcpy(hp, fp, static_cast<std::size_t>(S));
  auto t1 = Clock::now();
  double secs = ns_between(t0, t1) / 1e9;
  double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                (1024.0 * 1024.0) / secs;
  const bool intact = (static_cast<unsigned char>(hp[0]) == 0x3A);
  g_sink += intact ? 1u : 0u;
  report_metric("file_read_bandwidth", "read", mbps, "MiB/s");
}

void bench_view_creation_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 1 * MiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  auto parent = rt.allocate(req);
  if (!parent.ok()) { report_skip("view_creation_latency", "alloc failed"); return; }
  auto op = [&]() {
    auto v = parent.value().view(0, 1 * MiB, AccessMode::READ);
    if (v.ok()) { g_sink += v.value().size(); v.value().release(); } else { g_sink += 1; }
  };
  double ns = ns_per_op(op, 100000, 1000);
  report_metric("view_creation_latency", "view+release", ns, "ns/op");
}

void bench_metadata_lookup_latency(Runtime& rt) {
  AllocationRequest req;
  req.size = 8 * KiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = true;
  std::vector<BufferHandle> keep;
  for (int i = 0; i < 8; ++i) {
    auto h = try_alloc(rt, req);
    if (h.valid()) keep.push_back(std::move(h));
  }
  auto op = [&]() {
    auto st = rt.stats();
    if (st.ok()) { g_sink += st.value().active_buffers; } else { g_sink += 1; }
  };
  double ns = ns_per_op(op, 100000, 1000);
  report_metric("metadata_lookup_latency", "stats()", ns, "ns/op");
}

void bench_concurrent_alloc_throughput(Runtime& rt) {
  AllocationRequest req;
  req.size = 64 * KiB;
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = false;
  req.flags.pooled = false;
  const int counts[] = {1, 2, 4, 8};
  const long long per_thread = 30000;
  for (int nc : counts) {
    std::atomic<long long> total{0};
    std::vector<std::thread> threads;
    auto t0 = Clock::now();
    for (int t = 0; t < nc; ++t) {
      threads.emplace_back([&]() {
        for (long long i = 0; i < per_thread; ++i) {
          auto h = try_alloc(rt, req);
          if (h.valid()) total.fetch_add(1);
          h.release();
        }
      });
    }
    for (auto& th : threads) th.join();
    auto t1 = Clock::now();
    double secs = ns_between(t0, t1) / 1e9;
    double ops = static_cast<double>(total.load()) / secs;
    report_metric("concurrent_alloc_throughput", "threads=" + std::to_string(nc), ops, "ops/sec");
  }
}

#ifdef UNIFIED_BUFFER_HAS_CUDA
void bench_cuda(Runtime& rt) {
  if (!cuda_device_available(rt)) {
    report_skip("device_alloc_latency", "no CUDA device");
    report_skip("device_free_latency", "no CUDA device");
    report_skip("h2d_bandwidth", "no CUDA device");
    report_skip("d2h_bandwidth", "no CUDA device");
    report_skip("d2d_bandwidth", "no CUDA device");
    report_skip("pageable_staging_bandwidth", "no CUDA device");
    report_skip("pinned_staging_bandwidth", "no CUDA device");
    return;
  }

  constexpr std::uint64_t S = 32 * MiB;
  const long long iters = 20;

  // ---- device alloc / free latency ----
  AllocationRequest dreq;
  dreq.size = 16 * MiB;
  dreq.domain = MemoryDomain::DEVICE;
  dreq.device = DeviceId{BackendId::CUDA, 0};
  dreq.flags.zero_on_alloc = false;
  dreq.flags.pooled = false;
  double ns = ns_per_op([&]() {
    auto h = try_alloc(rt, dreq);
    if (h.valid()) { g_sink += h.size(); } else { g_sink += 1; }
    h.release();
  }, 500, 20);
  report_metric("device_alloc_latency", "allocate+release", ns, "ns/op");
  double fns = free_only_ns(rt, dreq, 500, 20);
  report_metric("device_free_latency", "release", fns, "ns/op");

  // ---- device + host staging buffers ----
  AllocationRequest dbig;
  dbig.size = S;
  dbig.domain = MemoryDomain::DEVICE;
  dbig.device = DeviceId{BackendId::CUDA, 0};
  dbig.flags.zero_on_alloc = false;
  dbig.flags.pooled = false;
  auto d = rt.allocate(dbig);
  if (!d.ok()) {
    report_skip("h2d_bandwidth", "device alloc failed");
    report_skip("d2h_bandwidth", "device alloc failed");
    report_skip("d2d_bandwidth", "device alloc failed");
    report_skip("pageable_staging_bandwidth", "device alloc failed");
    report_skip("pinned_staging_bandwidth", "device alloc failed");
    return;
  }

  AllocationRequest hreq;
  hreq.size = S;
  hreq.domain = MemoryDomain::HOST;
  hreq.flags.zero_on_alloc = false;
  hreq.flags.pooled = false;
  auto hs = rt.allocate(hreq);
  if (hs.ok()) {
    auto hmap = hs.value().map(AccessMode::READ_WRITE);
    if (hmap.ok()) {
      char* hp = static_cast<char*>(hmap.value().data());
      std::memset(hp, 0x5A, static_cast<std::size_t>(S));

      Status wst;
      for (long long w = 0; w < 3; ++w) wst = d.value().copy_from(hp, 0, S);
      if (wst.ok()) {
        auto t0 = Clock::now();
        for (long long i = 0; i < iters; ++i) d.value().copy_from(hp, 0, S);
        auto t1 = Clock::now();
        double secs = ns_between(t0, t1) / 1e9;
        double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                      (1024.0 * 1024.0) / secs;
        report_metric("h2d_bandwidth", "host->device", mbps, "MiB/s");
      } else {
        report_skip("h2d_bandwidth", "copy_from failed");
      }

      Status wst2;
      for (long long w = 0; w < 3; ++w) wst2 = d.value().copy_to(hp, 0, S);
      if (wst2.ok()) {
        auto t2 = Clock::now();
        for (long long i = 0; i < iters; ++i) d.value().copy_to(hp, 0, S);
        auto t3 = Clock::now();
        double secs = ns_between(t2, t3) / 1e9;
        double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                      (1024.0 * 1024.0) / secs;
        const bool intact = (static_cast<unsigned char>(hp[0]) == 0x5A);
        g_sink += intact ? 1u : 0u;
        report_metric("d2h_bandwidth", "device->host", mbps, "MiB/s");
      } else {
        report_skip("d2h_bandwidth", "copy_to failed");
      }

      Status wst3;
      for (long long w = 0; w < 3; ++w) wst3 = d.value().copy_from(hp, 0, S);
      if (wst3.ok()) {
        auto t4 = Clock::now();
        for (long long i = 0; i < iters; ++i) d.value().copy_from(hp, 0, S);
        auto t5 = Clock::now();
        double secs = ns_between(t4, t5) / 1e9;
        double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                      (1024.0 * 1024.0) / secs;
        report_metric("pageable_staging_bandwidth", "host->device", mbps, "MiB/s");
      } else {
        report_skip("pageable_staging_bandwidth", "copy_from failed");
      }
    } else {
      report_skip("h2d_bandwidth", "host map failed");
      report_skip("d2h_bandwidth", "host map failed");
      report_skip("pageable_staging_bandwidth", "host map failed");
    }
  } else {
    report_skip("h2d_bandwidth", "host alloc failed");
    report_skip("d2h_bandwidth", "host alloc failed");
    report_skip("pageable_staging_bandwidth", "host alloc failed");
  }

  // ---- d2d via raw device pointers ----
  auto d2 = rt.allocate(dbig);
  if (d2.ok()) {
    auto pa = d.value().device_pointer();
    auto pb = d2.value().device_pointer();
    if (pa.ok() && pb.ok()) {
      void* a = pa.value();
      void* b = pb.value();
      cudaMemcpy(b, a, static_cast<std::size_t>(S), cudaMemcpyDeviceToDevice);
      cudaDeviceSynchronize();
      auto t6 = Clock::now();
      for (long long i = 0; i < iters; ++i) {
        cudaMemcpy(b, a, static_cast<std::size_t>(S), cudaMemcpyDeviceToDevice);
        cudaDeviceSynchronize();
      }
      auto t7 = Clock::now();
      double secs = ns_between(t6, t7) / 1e9;
      double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                    (1024.0 * 1024.0) / secs;
      report_metric("d2d_bandwidth", "device->device", mbps, "MiB/s");
    } else {
      report_skip("d2d_bandwidth", "device_pointer failed");
    }
  } else {
    report_skip("d2d_bandwidth", "device alloc failed");
  }

  // ---- pinned vs pageable staging ----
  AllocationRequest preq;
  preq.size = S;
  preq.domain = MemoryDomain::PINNED_HOST;
  preq.flags.zero_on_alloc = false;
  preq.flags.pooled = false;
  auto pin = rt.allocate(preq);
  if (pin.ok()) {
    auto pm = pin.value().map(AccessMode::READ_WRITE);
    if (pm.ok()) {
      char* pp = static_cast<char*>(pm.value().data());
      std::memset(pp, 0x3C, static_cast<std::size_t>(S));
      Status wst4;
      for (long long w = 0; w < 3; ++w) wst4 = d.value().copy_from(pp, 0, S);
      if (wst4.ok()) {
        auto t8 = Clock::now();
        for (long long i = 0; i < iters; ++i) d.value().copy_from(pp, 0, S);
        auto t9 = Clock::now();
        double secs = ns_between(t8, t9) / 1e9;
        double mbps = (static_cast<double>(S) * static_cast<double>(iters)) /
                      (1024.0 * 1024.0) / secs;
        report_metric("pinned_staging_bandwidth", "host->device", mbps, "MiB/s");
      } else {
        report_skip("pinned_staging_bandwidth", "copy_from failed");
      }
    } else {
      report_skip("pinned_staging_bandwidth", "pinned map failed");
    }
  } else {
    report_skip("pinned_staging_bandwidth", "pinned host alloc failed");
  }
}
#endif  // UNIFIED_BUFFER_HAS_CUDA

}  // anonymous namespace

int main() {
  std::cout << "=== unified_buffer benchmark ===" << std::endl;
  std::cout << std::left << std::setw(28) << "name" << std::setw(20) << "measure"
            << std::right << std::setw(16) << "value" << "  unit" << std::endl;
  std::cout << std::string(72, '-') << std::endl;

  Runtime rt;

  bench_host_alloc_latency(rt);
  bench_host_free_latency(rt);
  bench_pooled_alloc_latency(rt);
  bench_pooled_free_latency(rt);
  bench_aligned_alloc_latency(rt);
  bench_small_buf_throughput(rt);
  bench_large_buf_throughput(rt);
  bench_pool_hit_rate();
  bench_host_memcpy_bandwidth(rt);

  if (domain_available(rt, MemoryDomain::SHARED_HOST)) {
    bench_shared_map_latency(rt);
    bench_shared_copy_bandwidth(rt);
  } else {
    report_skip("shared_map_latency", "SHARED_HOST domain unavailable");
    report_skip("shared_copy_bandwidth", "SHARED_HOST domain unavailable");
  }

  if (domain_available(rt, MemoryDomain::MMAP_STORAGE)) {
    bench_file_map_latency(rt);
    bench_file_write_bandwidth(rt);
    bench_file_read_bandwidth(rt);
  } else {
    report_skip("file_map_latency", "MMAP_STORAGE domain unavailable");
    report_skip("file_write_bandwidth", "MMAP_STORAGE domain unavailable");
    report_skip("file_read_bandwidth", "MMAP_STORAGE domain unavailable");
  }

  bench_view_creation_latency(rt);
  bench_metadata_lookup_latency(rt);
  bench_concurrent_alloc_throughput(rt);

#ifdef UNIFIED_BUFFER_HAS_CUDA
  bench_cuda(rt);
#else
  report_skip("device_alloc_latency", "CUDA not compiled in");
  report_skip("device_free_latency", "CUDA not compiled in");
  report_skip("h2d_bandwidth", "CUDA not compiled in");
  report_skip("d2h_bandwidth", "CUDA not compiled in");
  report_skip("d2d_bandwidth", "CUDA not compiled in");
  report_skip("pageable_staging_bandwidth", "CUDA not compiled in");
  report_skip("pinned_staging_bandwidth", "CUDA not compiled in");
#endif

  std::cout << std::endl << "=== MACHINE-READABLE SUMMARY ===" << std::endl;
  for (const auto& m : g_metrics) {
    std::cout << "bench," << m.name << "," << m.measure << "," << std::fixed
              << std::setprecision(6) << m.value << "," << m.unit << std::endl;
  }
  for (const auto& s : g_skipped) {
    std::cout << "bench," << s << ",skipped,-,na" << std::endl;
  }
  std::cout << std::endl << "RAN " << g_metrics.size() << " metrics; SKIPPED "
            << g_skipped.size() << "." << std::endl;
  std::cout << "sink=" << static_cast<unsigned long long>(g_sink) << std::endl;
  return 0;
}
