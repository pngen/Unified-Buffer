#include "common.hpp"
#include <cstdint>
#include <cstring>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime rt;
  auto before = rt.stats();
  if (!before.ok()) { ubex::print("[basic_host] stats() failed"); return 1; }
  ubex::print("[basic_host] active_buffers before = " + std::to_string(before.value().active_buffers));

  AllocationRequest req;
  req.size = 4ULL * 1024 * 1024;               // 4 MiB
  req.domain = MemoryDomain::HOST;
  req.flags.zero_on_alloc = true;
  req.flags.pooled = false;                    // free, not pool, so accounting returns to 0
  req.label = "basic-host";

  auto ar = rt.allocate(req);
  if (!ar.ok()) { ubex::print("[basic_host] allocate failed: " + ubex::errstr(ar)); return 1; }
  BufferHandle h = std::move(ar.value());
  const std::uint64_t n = h.size();
  ubex::print("[basic_host] allocated " + std::to_string(n) + " bytes in domain " +
              std::string(to_string(h.domain())));

  // Map and write a pattern.
  auto m = h.map(AccessMode::READ_WRITE);
  if (!m.ok()) { ubex::print("[basic_host] map failed: " + ubex::errstr(m)); return 1; }
  unsigned char* p = static_cast<unsigned char*>(m.value().data());
  for (std::uint64_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(i * 31 + 7);
  m.value().release();

  // Read back through a view and compare.
  auto v = h.view(0, n, AccessMode::READ);
  if (!v.ok()) { ubex::print("[basic_host] view failed: " + ubex::errstr(v)); return 1; }
  const unsigned char* q = static_cast<const unsigned char*>(v.value().data());
  bool pattern_ok = true;
  for (std::uint64_t i = 0; i < n; ++i)
    if (q[i] != static_cast<unsigned char>(i * 31 + 7)) { pattern_ok = false; break; }
  v.value().release();
  ubex::print(std::string("[basic_host] pattern read-back ") + (pattern_ok ? "OK" : "MISMATCH"));
  if (!pattern_ok) return 1;

  // Checksum is stable, integrity verify passes.
  auto c1 = h.checksum();
  auto c2 = h.checksum();
  if (!c1.ok() || !c2.ok()) { ubex::print("[basic_host] checksum failed"); return 1; }
  if (c1.value() != c2.value()) { ubex::print("[basic_host] checksum unstable"); return 1; }
  ubex::print("[basic_host] checksum = 0x" + ubex::hex32(c1.value()));

  auto vf = h.verify();
  ubex::print(std::string("[basic_host] verify() ") +
              (vf.ok() ? "OK" : ("FAILED: " + std::string(to_string(vf.error())))));
  if (!vf.ok()) return 1;

  h.release();
  auto after = rt.stats();
  if (!after.ok()) { ubex::print("[basic_host] stats() failed after release"); return 1; }
  ubex::print("[basic_host] active_buffers after = " + std::to_string(after.value().active_buffers) +
              ", total_frees = " + std::to_string(after.value().total_frees) +
              ", host_bytes = " + std::to_string(after.value().host_bytes));
  if (after.value().active_buffers != 0) { ubex::print("[basic_host] FAIL: active_buffers not back to 0"); return 1; }
  ubex::print("[basic_host] PASS");
  return 0;
}
