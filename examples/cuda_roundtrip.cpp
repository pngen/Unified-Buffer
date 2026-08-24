#include "common.hpp"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
using namespace unified_buffer;

int main() {
  Runtime rt;

  auto devs = rt.devices();
  if (!devs.ok() || devs.value().empty()) {
    ubex::print("[cuda_roundtrip] SKIP: no CUDA device available");
    return 0;
  }
  const int dev_index = devs.value().front().index;
  ubex::print("[cuda_roundtrip] using CUDA device index " + std::to_string(dev_index));

  const std::uint64_t n = 4096;
  std::vector<unsigned char> pattern(n);
  for (std::uint64_t i = 0; i < n; ++i) pattern[i] = static_cast<unsigned char>(i * 7 + 3);

  auto make = [&](MemoryDomain d) {
    AllocationRequest req;
    req.size = n;
    req.domain = d;
    req.flags.pooled = false;
    req.flags.zero_on_alloc = true;
    if (d == MemoryDomain::DEVICE) req.device = DeviceId{BackendId::CUDA, dev_index};
    return rt.allocate(req);
  };

  // Host source.
  auto hr = make(MemoryDomain::HOST);
  if (!hr.ok()) { ubex::print("[cuda_roundtrip] host allocate failed: " + ubex::errstr(hr)); return 1; }
  BufferHandle host = std::move(hr.value());
  auto hm = host.map(AccessMode::READ_WRITE);
  if (!hm.ok()) { ubex::print("[cuda_roundtrip] host map failed: " + ubex::errstr(hm)); return 1; }
  std::memcpy(hm.value().data(), pattern.data(), n);
  hm.value().release();

  // PINNED_HOST staging.
  auto pr = make(MemoryDomain::PINNED_HOST);
  if (!pr.ok()) { ubex::print("[cuda_roundtrip] pinned allocate failed: " + ubex::errstr(pr)); return 1; }
  BufferHandle pinned = std::move(pr.value());
  auto cp1 = pinned.copy_from(host.host_data(), 0, n);            // host -> pinned
  if (!cp1.ok()) { ubex::print("[cuda_roundtrip] host->pinned copy_from failed: " + ubex::errstr(cp1)); return 1; }

  // DEVICE destination.
  auto dr = make(MemoryDomain::DEVICE);
  if (!dr.ok()) { ubex::print("[cuda_roundtrip] device allocate failed: " + ubex::errstr(dr)); return 1; }
  BufferHandle device = std::move(dr.value());
  auto cp2 = device.copy_from(pinned.host_data(), 0, n);          // pinned -> device
  if (!cp2.ok()) { ubex::print("[cuda_roundtrip] pinned->device copy_from failed: " + ubex::errstr(cp2)); return 1; }

  // Device content back through a second pinned staging buffer, then to host.
  auto pr2 = make(MemoryDomain::PINNED_HOST);
  if (!pr2.ok()) { ubex::print("[cuda_roundtrip] pinned(back) allocate failed: " + ubex::errstr(pr2)); return 1; }
  BufferHandle pinned2 = std::move(pr2.value());
  void* pd = const_cast<void*>(pinned2.host_data());
  auto cp3 = device.copy_to(pd, 0, n);                            // device -> pinned
  if (!cp3.ok()) { ubex::print("[cuda_roundtrip] device->pinned copy_to failed: " + ubex::errstr(cp3)); return 1; }

  std::vector<unsigned char> final_dest(n);
  auto cp4 = pinned2.copy_to(final_dest.data(), 0, n);            // pinned -> host
  if (!cp4.ok()) { ubex::print("[cuda_roundtrip] pinned->host copy_to failed: " + ubex::errstr(cp4)); return 1; }

  bool exact = (final_dest == pattern);
  ubex::print(std::string("[cuda_roundtrip] host->pinned->device->pinned->host bytes exact = ") + (exact ? "yes" : "no"));
  if (!exact) return 1;

  host.release();
  pinned.release();
  device.release();
  pinned2.release();

  auto st = rt.stats();
  ubex::print("[cuda_roundtrip] after free: outstanding_allocations=" + std::to_string(st.value().outstanding_allocations) +
              " device_bytes=" + std::to_string(st.value().device_bytes));
  if (st.value().outstanding_allocations != 0 || st.value().device_bytes != 0) {
    ubex::print("[cuda_roundtrip] FAIL: accounting did not return to zero");
    return 1;
  }
  ubex::print("[cuda_roundtrip] PASS");
  return 0;
}
