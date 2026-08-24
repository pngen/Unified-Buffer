#include "common.hpp"
#include <cstdint>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime rt;

  // 1) Pool reuse: allocate 16 KiB host, release to the pool, allocate a match.
  AllocationRequest req;
  req.size = 16 * 1024;
  req.domain = MemoryDomain::HOST;
  req.flags.pooled = true;              // allow pool reuse
  req.flags.zero_on_alloc = true;
  req.label = "pool-reuse";

  auto s0 = rt.stats();
  auto r1 = rt.allocate(req);
  if (!r1.ok()) { ubex::print("[pool_reuse] first allocate failed: " + ubex::errstr(r1)); return 1; }
  BufferHandle h1 = std::move(r1.value());
  auto s1 = rt.stats();
  ubex::print("[pool_reuse] first allocate: pool_hits=" + std::to_string(s1.value().pool_hits) +
              " pool_misses=" + std::to_string(s1.value().pool_misses));

  h1.release();                         // goes back to the pool
  auto s2 = rt.stats();
  ubex::print("[pool_reuse] after release: idle_pooled=" + std::to_string(s2.value().idle_pooled_bytes) +
              " pooled=" + std::to_string(s2.value().pooled_bytes));

  auto r2 = rt.allocate(req);           // same size class -> should hit the pool
  if (!r2.ok()) { ubex::print("[pool_reuse] reuse allocate failed: " + ubex::errstr(r2)); return 1; }
  BufferHandle h2 = std::move(r2.value());
  auto s3 = rt.stats();
  ubex::print("[pool_reuse] reuse allocate: pool_hits=" + std::to_string(s3.value().pool_hits) +
              " pool_misses=" + std::to_string(s3.value().pool_misses));
  if (s3.value().pool_hits <= s2.value().pool_hits) {
    ubex::print("[pool_reuse] FAIL: pool_hits did not increase");
    return 1;
  }
  ubex::print("[pool_reuse] pool_hits increased from " + std::to_string(s2.value().pool_hits) +
              " to " + std::to_string(s3.value().pool_hits) + " (OK)");
  h2.release();

  // 2) Cross-owner zeroing: reuse a pooled block across two namespaces.
  NamespaceConfig n1; n1.name = "owner-a"; rt.create_namespace(n1);
  NamespaceConfig n2; n2.name = "owner-b"; rt.create_namespace(n2);

  AllocationRequest cr;
  cr.size = 16 * 1024;
  cr.domain = MemoryDomain::HOST;
  cr.flags.pooled = true;
  cr.flags.zero_on_alloc = false;       // write something, then rely on cross-owner zeroing
  cr.ns = 2;                            // owner-a
  auto ca = rt.allocate(cr);
  if (!ca.ok()) { ubex::print("[pool_reuse] owner-a allocate failed: " + ubex::errstr(ca)); return 1; }
  BufferHandle ha = std::move(ca.value());
  auto ma = ha.map(AccessMode::READ_WRITE);
  if (ma.ok()) {
    unsigned char* p = static_cast<unsigned char*>(ma.value().data());
    for (int i = 0; i < 16 * 1024; ++i) p[i] = 0xAA;   // put non-zero data in the recycled block
    ma.value().release();
  }
  ha.release();                          // block now idle in the pool, last_owner = owner-a

  cr.ns = 3;                             // owner-b
  auto cb = rt.allocate(cr);
  if (!cb.ok()) { ubex::print("[pool_reuse] owner-b allocate failed: " + ubex::errstr(cb)); return 1; }
  BufferHandle hb = std::move(cb.value());
  auto mb = hb.map(AccessMode::READ);
  bool zeroed = true;
  if (mb.ok()) {
    const unsigned char* p = static_cast<const unsigned char*>(mb.value().data());
    for (int i = 0; i < 16 * 1024; ++i) if (p[i] != 0) { zeroed = false; break; }
    mb.value().release();
  }
  ubex::print(std::string("[pool_reuse] cross-owner buffer zeroed on reuse = ") + (zeroed ? "yes" : "no"));
  hb.release();
  if (!zeroed) { ubex::print("[pool_reuse] FAIL: cross-owner reuse did not zero"); return 1; }

  ubex::print("[pool_reuse] PASS");
  return 0;
}
