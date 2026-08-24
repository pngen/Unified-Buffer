#include "common.hpp"
#include <cstdint>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime rt;

  const std::uint64_t n = 4096;
  AllocationRequest req;
  req.size = n;
  req.domain = MemoryDomain::HOST;
  req.flags.pooled = false;
  req.flags.zero_on_alloc = true;

  auto ar = rt.allocate(req);
  if (!ar.ok()) { ubex::print("[migrate] allocate failed: " + ubex::errstr(ar)); return 1; }
  BufferHandle h = std::move(ar.value());

  auto m = h.map(AccessMode::READ_WRITE);
  if (!m.ok()) { ubex::print("[migrate] map failed: " + ubex::errstr(m)); return 1; }
  unsigned char* p = static_cast<unsigned char*>(m.value().data());
  for (std::uint64_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(i * 5 + 9);
  m.value().release();

  const auto gen_before = h.generation();
  ubex::print("[migrate] source handle generation = " + std::to_string(gen_before));

  MemoryDomain target = MemoryDomain::DEVICE;
  if (!ubex::has_cuda(rt)) {
    ubex::print("[migrate] no CUDA - migrate to shared memory (SHARED_HOST)");
    target = MemoryDomain::SHARED_HOST;
  } else {
    ubex::print("[migrate] CUDA present - migrating to DEVICE");
  }

  auto mg = rt.migrate(h, target);
  if (!mg.ok()) { ubex::print("[migrate] migrate failed: " + ubex::errstr(mg)); return 1; }
  BufferHandle h2 = std::move(mg.value());

  ubex::print("[migrate] migrated handle: generation=" + std::to_string(h2.generation()) +
              " domain=" + std::string(to_string(h2.domain())));
  if (h2.generation() <= gen_before) { ubex::print("[migrate] FAIL: generation did not increase"); return 1; }
  ubex::print("[migrate] generation increased from " + std::to_string(gen_before) + " to " +
              std::to_string(h2.generation()) + " (OK)");

  auto stale_map = h.map(AccessMode::READ);
  ubex::print(std::string("[migrate] old handle is stale on use = ") +
              (stale_map.ok() ? "no (unexpected)" : ("yes (" + std::string(to_string(stale_map.error())) + ")")));
  if (stale_map.ok()) { ubex::print("[migrate] FAIL: old handle should be rejected"); return 1; }

  // Verify content survived by copying the migrated buffer back to a host buffer.
  auto vr = rt.allocate(req);
  if (!vr.ok()) { ubex::print("[migrate] verify host allocate failed: " + ubex::errstr(vr)); return 1; }
  BufferHandle verif = std::move(vr.value());
  auto vm = verif.map(AccessMode::READ_WRITE);
  if (!vm.ok()) { ubex::print("[migrate] verify host map failed: " + ubex::errstr(vm)); return 1; }
  auto ct = h2.copy_to(vm.value().data(), 0, n);
  if (!ct.ok()) { ubex::print("[migrate] copy_to of migrated content failed: " + ubex::errstr(ct)); return 1; }
  vm.value().release();

  auto vv = verif.map(AccessMode::READ);
  if (!vv.ok()) { ubex::print("[migrate] verify host re-map failed: " + ubex::errstr(vv)); return 1; }
  const unsigned char* q = static_cast<const unsigned char*>(vv.value().data());
  bool same = true;
  for (std::uint64_t i = 0; i < n; ++i) if (q[i] != static_cast<unsigned char>(i * 5 + 9)) { same = false; break; }
  vv.value().release();
  ubex::print(std::string("[migrate] content survived migration = ") + (same ? "yes" : "no"));
  if (!same) return 1;

  h2.release();
  h.release();
  verif.release();
  ubex::print("[migrate] PASS");
  return 0;
}
