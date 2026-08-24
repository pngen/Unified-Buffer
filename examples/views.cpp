#include "common.hpp"
#include <cstdint>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime rt;
  const std::uint64_t total = 4096;
  AllocationRequest req;
  req.size = total;
  req.domain = MemoryDomain::HOST;
  req.flags.pooled = false;
  req.flags.zero_on_alloc = true;

  auto ar = rt.allocate(req);
  if (!ar.ok()) { ubex::print("[views] allocate failed: " + ubex::errstr(ar)); return 1; }
  BufferHandle parent = std::move(ar.value());
  ubex::print("[views] parent buffer size=" + std::to_string(parent.size()));

  // Two non-overlapping views.
  const std::uint64_t half = total / 2;
  auto va = parent.view(0, half, AccessMode::WRITE);
  auto vb = parent.view(half, half, AccessMode::WRITE);
  if (!va.ok() || !vb.ok()) { ubex::print("[views] creating views failed"); return 1; }

  unsigned char* pa = static_cast<unsigned char*>(const_cast<void*>(va.value().data()));
  unsigned char* pb = static_cast<unsigned char*>(const_cast<void*>(vb.value().data()));
  for (std::uint64_t i = 0; i < half; ++i) { pa[i] = 0xA5; pb[i] = 0x5A; }

  // Verify the parent memory reflects both distinct patterns in their ranges.
  auto vm = parent.map(AccessMode::READ);
  if (!vm.ok()) { ubex::print("[views] parent map failed: " + ubex::errstr(vm)); return 1; }
  const unsigned char* pm = static_cast<const unsigned char*>(vm.value().data());
  bool a_ok = true, b_ok = true;
  for (std::uint64_t i = 0; i < half; ++i) {
    if (pm[i] != 0xA5) { a_ok = false; break; }
  }
  for (std::uint64_t i = half; i < total; ++i) {
    if (pm[i] != 0x5A) { b_ok = false; break; }
  }
  vm.value().release();
  ubex::print("[views] view A pattern (0x00-" + std::to_string(half) + ") = " + std::string(a_ok ? "0xA5 OK" : "MISMATCH"));
  ubex::print("[views] view B pattern (" + std::to_string(half) + "-" + std::to_string(total) + ") = " + std::string(b_ok ? "0x5A OK" : "MISMATCH"));
  if (!a_ok || !b_ok) return 1;
  ubex::print("[views] view offsets/length: A(" + std::to_string(va.value().offset()) + "," +
              std::to_string(va.value().length()) + ") B(" + std::to_string(vb.value().offset()) + "," +
              std::to_string(vb.value().length()) + ")");

  va.value().release();
  vb.value().release();

  // Invalid slice: offset + length exceeds the buffer -> rejected as bounds_error.
  auto bad = parent.view(3000, 2000, AccessMode::READ);
  ubex::print(std::string("[views] invalid slice (offset 3000, len 2000) rejected = ") +
              (bad.ok() ? "no (unexpected)" : ("yes (" + std::string(to_string(bad.error())) + ")")));
  if (bad.ok()) { ubex::print("[views] FAIL: out-of-range slice was accepted"); return 1; }

  parent.release();
  ubex::print("[views] PASS");
  return 0;
}