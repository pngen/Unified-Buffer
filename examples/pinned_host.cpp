#include "common.hpp"
#include <cstdint>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime rt;

  // The PINNED_HOST domain is only present when a CUDA backend is compiled in
  // AND a CUDA runtime/device is available at runtime.
  if (!ubex::domain_enabled(rt, MemoryDomain::PINNED_HOST)) {
    ubex::print("[pinned_host] SKIP: PINNED_HOST domain unavailable (no CUDA device/runtime)");
    return 0;
  }

  const std::uint64_t n = 1024;                 // keep below pool min class so it frees cleanly
  AllocationRequest req;
  req.size = n;
  req.domain = MemoryDomain::PINNED_HOST;
  req.flags.pooled = false;                     // free on release so accounting returns to 0
  req.flags.zero_on_alloc = true;
  req.label = "pinned-host";

  auto r = rt.allocate(req);
  if (!r.ok()) { ubex::print("[pinned_host] allocate failed: " + ubex::errstr(r)); return 1; }
  BufferHandle h = std::move(r.value());
  ubex::print("[pinned_host] allocated " + std::to_string(h.size()) + " bytes in " +
              std::string(to_string(h.domain())));

  auto m = h.map(AccessMode::READ_WRITE);
  if (!m.ok()) { ubex::print("[pinned_host] map failed: " + ubex::errstr(m)); return 1; }
  unsigned char* p = static_cast<unsigned char*>(m.value().data());
  for (std::uint64_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(i * 13 + 5);
  m.value().release();

  auto m2 = h.map(AccessMode::READ);
  bool ok = true;
  if (m2.ok()) {
    const unsigned char* q = static_cast<const unsigned char*>(m2.value().data());
    for (std::uint64_t i = 0; i < n; ++i) if (q[i] != static_cast<unsigned char>(i * 13 + 5)) { ok = false; break; }
    m2.value().release();
  }
  ubex::print(std::string("[pinned_host] write/read back ") + (ok ? "OK" : "MISMATCH"));
  if (!ok) return 1;

  auto vf = h.verify();
  ubex::print(std::string("[pinned_host] verify() ") + (vf.ok() ? "OK" : "FAILED"));
  if (!vf.ok()) return 1;

  h.release();
  auto st = rt.stats();
  ubex::print("[pinned_host] after release: pinned_host_bytes=" + std::to_string(st.value().pinned_host_bytes) +
              " active_buffers=" + std::to_string(st.value().active_buffers));
  if (st.value().pinned_host_bytes != 0) { ubex::print("[pinned_host] FAIL: pinned_host_bytes not back to 0"); return 1; }
  if (st.value().active_buffers != 0) { ubex::print("[pinned_host] FAIL: active_buffers not 0"); return 1; }
  ubex::print("[pinned_host] PASS");
  return 0;
}
