#include "common.hpp"
#include <cstdint>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime rt;

  // Create a namespace with a small host quota.  The default namespace is id 1,
  // so the first namespace we create gets id 2.
  NamespaceConfig ns;
  ns.name = "quota-ns";
  ns.host_quota = 4096;                 // bytes of HOST memory allowed in this namespace
  auto cs = rt.create_namespace(ns);
  if (!cs.ok()) { ubex::print("[quota] create_namespace failed: " + ubex::errstr(cs)); return 1; }
  const NamespaceId qns = 2;

  AllocationRequest req;
  req.ns = qns;
  req.size = 4096;
  req.domain = MemoryDomain::HOST;
  req.flags.pooled = false;
  req.flags.zero_on_alloc = true;

  // Fill the quota with one allocation.
  auto r1 = rt.allocate(req);
  if (!r1.ok()) { ubex::print("[quota] first allocate failed: " + ubex::errstr(r1)); return 1; }
  BufferHandle h1 = std::move(r1.value());
  ubex::print("[quota] allocated " + std::to_string(h1.size()) + " bytes in quota namespace");

  // One more should be rejected with quota_exceeded (and must not leak).
  auto r2 = rt.allocate(req);
  ubex::print(std::string("[quota] second allocation rejected = ") +
              (r2.ok() ? "no (unexpected)" : ("yes (" + std::string(to_string(r2.error())) + ")")));
  if (r2.ok()) { ubex::print("[quota] FAIL: over-quota allocation was accepted"); return 1; }
  if (r2.error() != ErrorCode::quota_exceeded) {
    ubex::print("[quota] FAIL: expected quota_exceeded, got " + std::string(to_string(r2.error())));
    return 1;
  }

  h1.release();

  auto st = rt.stats();
  ubex::print("[quota] after cleanup: active_buffers=" + std::to_string(st.value().active_buffers) +
              " host_bytes=" + std::to_string(st.value().host_bytes) +
              " quota_rejections=" + std::to_string(st.value().quota_rejections));
  if (st.value().active_buffers != 0 || st.value().host_bytes != 0) {
    ubex::print("[quota] FAIL: accounting did not return to baseline");
    return 1;
  }
  if (st.value().quota_rejections < 1) { ubex::print("[quota] FAIL: quota_rejections was not recorded"); return 1; }
  ubex::print("[quota] PASS");
  return 0;
}
