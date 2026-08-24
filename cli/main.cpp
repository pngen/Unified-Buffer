#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/export.hpp"
#include "unified_buffer/telemetry.hpp"
#include "unified_buffer/version.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

using namespace unified_buffer;

namespace {

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

void print_domain_info(const DomainInfo& d) {
  std::printf("  %-14s backend=%-16s enabled=%d caps=%llu total=%llu free=%llu\n",
    d.name.c_str(), std::string(to_string(d.backend)).c_str(), (int)d.enabled,
    (unsigned long long)d.capabilities.mask(), (unsigned long long)d.total_capacity, (unsigned long long)d.free);
}

void print_stats(const RuntimeStats& s) {
  std::printf("version=%s closed=%d\n", s.version.c_str(), (int)s.closed);
  std::printf("active_buffers=%llu outstanding_allocations=%llu\n", (unsigned long long)s.active_buffers, (unsigned long long)s.outstanding_allocations);
  std::printf("total_allocations=%llu total_frees=%llu failed_allocations=%llu\n", (unsigned long long)s.total_allocations, (unsigned long long)s.total_frees, (unsigned long long)s.failed_allocations);
  std::printf("bytes_requested=%llu bytes_committed=%llu peak_bytes=%llu\n", (unsigned long long)s.bytes_requested, (unsigned long long)s.bytes_committed, (unsigned long long)s.peak_bytes);
  std::printf("host=%llu pinned=%llu device=%llu shared=%llu file=%llu\n", (unsigned long long)s.host_bytes, (unsigned long long)s.pinned_host_bytes, (unsigned long long)s.device_bytes, (unsigned long long)s.shared_host_bytes, (unsigned long long)s.file_backed_bytes);
  std::printf("pooled=%llu idle_pooled=%llu pool_hits=%llu pool_misses=%llu pool_evictions=%llu\n", (unsigned long long)s.pooled_bytes, (unsigned long long)s.idle_pooled_bytes, (unsigned long long)s.pool_hits, (unsigned long long)s.pool_misses, (unsigned long long)s.pool_evictions);
  std::printf("map_count=%llu unmap_count=%llu export_count=%llu import_count=%llu\n", (unsigned long long)s.map_count, (unsigned long long)s.unmap_count, (unsigned long long)s.export_count, (unsigned long long)s.import_count);
  std::printf("copy_operations=%llu bytes_copied=%llu migration_count=%llu zero_operations=%llu\n", (unsigned long long)s.copy_operations, (unsigned long long)s.bytes_copied, (unsigned long long)s.migration_count, (unsigned long long)s.zero_operations);
  std::printf("integrity_checks=%llu integrity_failures=%llu stale_handle_rejections=%llu stale_generation_rejections=%llu\n", (unsigned long long)s.integrity_checks, (unsigned long long)s.integrity_failures, (unsigned long long)s.stale_handle_rejections, (unsigned long long)s.stale_generation_rejections);
  std::printf("quota_rejections=%llu lease_conflicts=%llu\n", (unsigned long long)s.quota_rejections, (unsigned long long)s.lease_conflicts);
}

void print_stats_json(const RuntimeStats& s) {
  std::printf("{\"version\":\"%s\",\"closed\":%d,\"active_buffers\":%llu,\"outstanding_allocations\":%llu,\"total_allocations\":%llu,\"total_frees\":%llu,\"failed_allocations\":%llu,\"bytes_requested\":%llu,\"bytes_committed\":%llu,\"peak_bytes\":%llu,\"host_bytes\":%llu,\"pinned_host_bytes\":%llu,\"device_bytes\":%llu,\"shared_host_bytes\":%llu,\"file_backed_bytes\":%llu,\"pooled_bytes\":%llu,\"idle_pooled_bytes\":%llu,\"pool_hits\":%llu,\"pool_misses\":%llu,\"pool_evictions\":%llu,\"map_count\":%llu,\"unmap_count\":%llu,\"export_count\":%llu,\"import_count\":%llu,\"migration_count\":%llu,\"copy_operations\":%llu,\"bytes_copied\":%llu,\"stale_handle_rejections\":%llu,\"stale_generation_rejections\":%llu,\"quota_rejections\":%llu,\"lease_conflicts\":%llu}\n",
    s.version.c_str(), (int)s.closed, (unsigned long long)s.active_buffers, (unsigned long long)s.outstanding_allocations, (unsigned long long)s.total_allocations, (unsigned long long)s.total_frees, (unsigned long long)s.failed_allocations, (unsigned long long)s.bytes_requested, (unsigned long long)s.bytes_committed, (unsigned long long)s.peak_bytes, (unsigned long long)s.host_bytes, (unsigned long long)s.pinned_host_bytes, (unsigned long long)s.device_bytes, (unsigned long long)s.shared_host_bytes, (unsigned long long)s.file_backed_bytes, (unsigned long long)s.pooled_bytes, (unsigned long long)s.idle_pooled_bytes, (unsigned long long)s.pool_hits, (unsigned long long)s.pool_misses, (unsigned long long)s.pool_evictions, (unsigned long long)s.map_count, (unsigned long long)s.unmap_count, (unsigned long long)s.export_count, (unsigned long long)s.import_count, (unsigned long long)s.migration_count, (unsigned long long)s.copy_operations, (unsigned long long)s.bytes_copied, (unsigned long long)s.stale_handle_rejections, (unsigned long long)s.stale_generation_rejections, (unsigned long long)s.quota_rejections, (unsigned long long)s.lease_conflicts);
}

void selftest(Runtime& rt) {
  AllocationRequest req; req.size = 1 << 20; req.domain = MemoryDomain::HOST; req.flags.pooled = false;
  auto r = rt.allocate(req);
  if (!r.ok()) { std::printf("selftest: allocation failed: %s\n", r.message().c_str()); return; }
  BufferHandle h = std::move(r.value());
  auto m = h.map(AccessMode::READ_WRITE);
  if (m.ok()) { unsigned char* p = static_cast<unsigned char*>(m.value().data()); for (int i = 0; i < (1 << 20); ++i) p[i] = (unsigned char)(i * 7 + 1); m.value().release(); }
  auto c = h.checksum();
  auto v = h.verify();
  h.release();
  auto s = rt.stats();
  bool check = s.ok() && s.value().active_buffers == 0 && s.value().host_bytes == 0;
  std::printf("selftest: %s (checksum=0x%x)\n", (c.ok() && v.ok() && check) ? "PASS" : "FAIL", c.ok() ? c.value() : 0);
}

void benchmark(Runtime& rt) {
  using clock = std::chrono::high_resolution_clock;
  const int iters = 200000;
  volatile unsigned char sink = 0;
  auto t0 = clock::now();
  for (int i = 0; i < iters; ++i) {
    AllocationRequest req; req.size = 4096; req.domain = MemoryDomain::HOST; req.flags.pooled = false;
    auto r = rt.allocate(req); if (!r.ok()) break;
    BufferHandle h = std::move(r.value());
    sink ^= *static_cast<volatile unsigned char*>(const_cast<void*>(h.host_data()));
    h.release();
  }
  auto t1 = clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::printf("bench,host_alloc_free_latency,%0.3f,ns/op,(%d iters)\n", ms * 1e6 / iters, iters);
  (void)sink;
}

}  // namespace

int main(int argc, char** argv) {
  Runtime rt;
  std::unordered_map<std::uint64_t, BufferHandle> handles;

  auto handle_cmd = [&](const std::vector<std::string>& toks, bool& alive) {
    alive = true;
    if (toks.empty()) { alive = false; return; }
    const std::string& cmd = toks[0];
    if (cmd == "help") {
      std::printf("commands: info backends devices domains stats pools alloc <size> [domain] [--nopool] free <idlo> inspect <idlo> verify <idlo> migrate <idlo> <domain> selftest benchmark stats --json quit\n");
    } else if (cmd == "info") {
      std::printf("unified-buffer %s\n", unified_buffer::version_string());
      auto s = rt.stats(); if (s.ok()) std::printf("active_buffers=%llu\n", (unsigned long long)s.value().active_buffers);
    } else if (cmd == "backends") {
      auto b = rt.backends(); if (b.ok()) for (auto& x : b.value()) std::printf("  %s enabled=%d\n", std::string(to_string(x.id)).c_str(), (int)x.enabled);
    } else if (cmd == "devices") {
      auto d = rt.devices(); if (d.ok()) for (auto& x : d.value()) std::printf("  %s[%d] name=%s total=%llu free=%llu cc=%d.%d avail=%d\n", std::string(to_string(x.backend)).c_str(), x.index, x.name.c_str(), (unsigned long long)x.total_memory, (unsigned long long)x.free_memory, x.compute_major, x.compute_minor, (int)x.available);
    } else if (cmd == "domains") {
      auto d = rt.domains(); if (d.ok()) for (auto& x : d.value()) print_domain_info(x);
    } else if (cmd == "stats") {
      auto s = rt.stats(); if (s.ok()) { if (toks.size() > 1 && toks[1] == "--json") print_stats_json(s.value()); else print_stats(s.value()); }
    } else if (cmd == "pools") {
      auto s = rt.stats(); if (s.ok()) std::printf("pooled_bytes=%llu idle_pooled_bytes=%llu pool_hits=%llu pool_misses=%llu evictions=%llu\n", (unsigned long long)s.value().pooled_bytes, (unsigned long long)s.value().idle_pooled_bytes, (unsigned long long)s.value().pool_hits, (unsigned long long)s.value().pool_misses, (unsigned long long)s.value().pool_evictions);
    } else if (cmd == "alloc") {
      if (toks.size() < 2) { std::printf("usage: alloc <size> [domain]\n"); return; }
      AllocationRequest req;
      req.size = std::strtoull(toks[1].c_str(), nullptr, 10);
      req.domain = MemoryDomain::HOST;
      if (toks.size() >= 3) {
        std::string d = toks[2];
        if (d == "host") req.domain = MemoryDomain::HOST;
        else if (d == "pinned") req.domain = MemoryDomain::PINNED_HOST;
        else if (d == "device") req.domain = MemoryDomain::DEVICE;
        else if (d == "shared") req.domain = MemoryDomain::SHARED_HOST;
        else if (d == "file") req.domain = MemoryDomain::MMAP_STORAGE;
      }
      for (auto& t : toks) if (t == "--nopool") req.flags.pooled = false;
      if (req.domain == MemoryDomain::DEVICE) { auto dv = rt.devices(); if (dv.ok() && !dv.value().empty()) req.device.index = dv.value()[0].index; }
      auto r = rt.allocate(req);
      if (!r.ok()) { std::printf("alloc failed: %s\n", r.message().c_str()); return; }
      BufferHandle h = std::move(r.value());
      std::uint64_t idlo = h.id().lo;
      handles[idlo] = std::move(h);
      std::printf("allocated id=%llu size=%llu domain=%s\n", (unsigned long long)idlo, (unsigned long long)req.size, std::string(to_string(req.domain)).c_str());
    } else if (cmd == "free") {
      if (toks.size() < 2) { std::printf("usage: free <idlo>\n"); return; }
      std::uint64_t idlo = std::strtoull(toks[1].c_str(), nullptr, 10);
      auto it = handles.find(idlo);
      if (it == handles.end()) { std::printf("no handle id=%llu\n", (unsigned long long)idlo); return; }
      it->second.release();
      handles.erase(it);
      std::printf("freed id=%llu\n", (unsigned long long)idlo);
    } else if (cmd == "inspect") {
      if (toks.size() < 2) { std::printf("usage: inspect <idlo>\n"); return; }
      std::uint64_t idlo = std::strtoull(toks[1].c_str(), nullptr, 10);
      auto it = handles.find(idlo);
      if (it == handles.end()) { std::printf("no handle id=%llu\n", (unsigned long long)idlo); return; }
      auto& h = it->second;
      std::printf("id=%llu gen=%llu domain=%s size=%llu align=%llu state=%s valid=%d\n", (unsigned long long)h.id().lo, (unsigned long long)h.generation(), std::string(to_string(h.domain())).c_str(), (unsigned long long)h.size(), (unsigned long long)h.alignment(), std::string(to_string(h.state())).c_str(), (int)h.valid());
    } else if (cmd == "verify") {
      if (toks.size() < 2) { std::printf("usage: verify <idlo>\n"); return; }
      std::uint64_t idlo = std::strtoull(toks[1].c_str(), nullptr, 10);
      auto it = handles.find(idlo);
      if (it == handles.end()) { std::printf("no handle id=%llu\n", (unsigned long long)idlo); return; }
      auto v = it->second.verify();
      std::printf("verify id=%llu: %s\n", (unsigned long long)idlo, v.ok() ? "PASS" : v.message().c_str());
    } else if (cmd == "migrate") {
      if (toks.size() < 3) { std::printf("usage: migrate <idlo> <domain>\n"); return; }
      std::uint64_t idlo = std::strtoull(toks[1].c_str(), nullptr, 10);
      auto it = handles.find(idlo); if (it == handles.end()) { std::printf("no handle id=%llu\n", (unsigned long long)idlo); return; }
      MemoryDomain target = MemoryDomain::SHARED_HOST;
      std::string d = toks[2];
      if (d == "device") target = MemoryDomain::DEVICE; else if (d == "host") target = MemoryDomain::HOST; else if (d == "file") target = MemoryDomain::MMAP_STORAGE;
      auto m = rt.migrate(it->second, target);
      if (!m.ok()) { std::printf("migrate failed: %s\n", m.message().c_str()); return; }
      BufferHandle h2 = std::move(m.value());
      handles[idlo] = std::move(h2);
      std::printf("migrated id=%llu -> %s new_gen=%llu\n", (unsigned long long)idlo, std::string(to_string(target)).c_str(), (unsigned long long)handles[idlo].generation());
    } else if (cmd == "selftest") {
      selftest(rt);
    } else if (cmd == "benchmark") {
      benchmark(rt);
    } else if (cmd == "quit" || cmd == "exit") {
      alive = false;
    } else {
      std::printf("unknown command: %s (try help)\n", cmd.c_str());
    }
  };

  if (argc >= 2 && std::string(argv[1]) == "--json" && argc >= 3 && std::string(argv[2]) == "stats") {
    auto s = rt.stats(); if (s.ok()) print_stats_json(s.value());
    return 0;
  }
  if (argc >= 2) {
    std::vector<std::string> toks(argv + 1, argv + argc);
    bool alive = true;
    handle_cmd(toks, alive);
    return 0;
  }

  // Interactive mode.
  std::printf("unified-buffer %s interactive shell.  Type 'help' for commands, 'quit' to exit.\n", unified_buffer::version_string());
  std::string line;
  while (std::getline(std::cin, line)) {
    bool alive = false;
    handle_cmd(split(line), alive);
    if (!alive) { std::printf("bye\n"); break; }
  }
  return 0;
}
