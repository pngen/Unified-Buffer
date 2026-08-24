#include "common.hpp"
#include <cstdint>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime rt;
  AllocationRequest req;
  req.size = 4096;
  req.domain = MemoryDomain::HOST;
  req.flags.pooled = false;
  req.flags.zero_on_alloc = true;

  auto ar = rt.allocate(req);
  if (!ar.ok()) { ubex::print("[stale_handle] allocate failed: " + ubex::errstr(ar)); return 1; }
  BufferHandle h = std::move(ar.value());
  const auto id = h.id();
  ubex::print("[stale_handle] allocated handle (id lo=" + std::to_string(id.lo) + ") valid=" +
              std::string(h.valid() ? "true" : "false"));

  // Release the buffer, then attempt to use the (now stale) handle.
  h.release();
  if (h.valid()) { ubex::print("[stale_handle] FAIL: handle still valid after release"); return 1; }

  auto mm = h.map(AccessMode::READ);
  ubex::print(std::string("[stale_handle] map() on released handle rejected = ") +
              (mm.ok() ? "no (unexpected)" : ("yes (" + std::string(to_string(mm.error())) + ")")));
  if (mm.ok()) { ubex::print("[stale_handle] FAIL: map on stale handle succeeded"); return 1; }

  auto vv = h.view(0, 4096, AccessMode::READ);
  ubex::print(std::string("[stale_handle] view() on released handle rejected = ") +
              (vv.ok() ? "no (unexpected)" : ("yes (" + std::string(to_string(vv.error())) + ")")));
  if (vv.ok()) { ubex::print("[stale_handle] FAIL: view on stale handle succeeded"); return 1; }

  // A moved-from handle is invalid, and any use of it is rejected.
  auto ar2 = rt.allocate(req);
  if (!ar2.ok()) { ubex::print("[stale_handle] second allocate failed: " + ubex::errstr(ar2)); return 1; }
  BufferHandle orig = std::move(ar2.value());
  BufferHandle moved = std::move(orig);
  ubex::print("[stale_handle] moved-to handle valid=" + std::string(moved.valid() ? "true" : "false") +
              ", moved-from handle valid=" + std::string(orig.valid() ? "true" : "false"));
  if (moved.valid() != true || orig.valid() != false) {
    ubex::print("[stale_handle] FAIL: move semantics wrong");
    return 1;
  }
  auto om = orig.map(AccessMode::READ);
  ubex::print(std::string("[stale_handle] map() on moved-from handle rejected = ") +
              (om.ok() ? "no (unexpected)" : ("yes (" + std::string(to_string(om.error())) + ")")));
  if (om.ok()) { ubex::print("[stale_handle] FAIL: map on moved-from handle succeeded"); return 1; }

  moved.release();
  ubex::print("[stale_handle] PASS");
  return 0;
}
