#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "test_framework.hpp"
#include <cstring>
#include <vector>
#include <cstdio>

using namespace unified_buffer;

static void cuda_roundtrip() {
  Runtime rt;
  // Device identity.
  auto devs = rt.devices();
  if (!devs.ok() || devs.value().empty() || !devs.value()[0].available) {
    std::printf("  [skip] CUDA device not available\n");
    return;
  }
  std::uint64_t n = 1 << 20;
  // Device buffer.
  AllocationRequest dreq; dreq.size = n; dreq.domain = MemoryDomain::DEVICE; dreq.device.index = devs.value()[0].index; dreq.flags.zero_on_alloc = false; dreq.flags.pooled = false;
  auto rd = rt.allocate(dreq); CHECK_TRUE(rd.ok());
  if (!rd.ok()) return;
  BufferHandle dev = std::move(rd.value());
  // Pinned staging.
  AllocationRequest preq; preq.size = n; preq.domain = MemoryDomain::PINNED_HOST; preq.flags.zero_on_alloc = false; preq.flags.pooled = false;
  auto rp = rt.allocate(preq); CHECK_TRUE(rp.ok());
  if (!rp.ok()) return;
  BufferHandle pin = std::move(rp.value());
  BufferHandle pin2 = std::move(rt.allocate(preq).value());
  // Host source / sink.
  std::vector<unsigned char> src(n), dst(n);
  for (uint64_t i = 0; i < n; ++i) src[i] = (unsigned char)(i * 3 + 1);

  // host -> pinned
  CHECK_TRUE(pin.copy_from(src.data(), 0, n).ok());
  // pinned -> device
  CHECK_TRUE(dev.copy_from(pin.host_data(), 0, n).ok());
  // device -> pinned2 (via an explicit writable map)
  auto pm = pin2.map(AccessMode::READ_WRITE);
  CHECK_TRUE(pm.ok());
  if (pm.ok()) CHECK_TRUE(dev.copy_to(pm.value().data(), 0, n).ok());
  pm.value().release();
  // pinned2 -> host dst
  CHECK_TRUE(pin2.copy_to(dst.data(), 0, n).ok());
  CHECK_TRUE(dst == src);

  CHECK_TRUE(dev.verify().ok());
  uint64_t gen_before = dev.generation();
  // Device pointer borrowed access.
  auto dp = dev.device_pointer();
  CHECK_TRUE(dp.ok());
  if (dp.ok()) CHECK_TRUE(dp.value() != nullptr);

  // Cleanup.
  CHECK_TRUE(dev.release().ok());
  CHECK_TRUE(pin.release().ok());
  CHECK_TRUE(pin2.release().ok());

  auto s = rt.stats();
  CHECK_TRUE(s.ok());
  if (s.ok()) {
    CHECK_EQ((long long)s.value().outstanding_allocations, 0);
    CHECK_EQ((long long)s.value().device_bytes, 0);
    CHECK_EQ((long long)s.value().pinned_host_bytes, 0);
  }
  (void)gen_before;
}

static void pinned_capacity() {
  Runtime rt;
  AllocationRequest preq; preq.size = 1 << 20; preq.domain = MemoryDomain::PINNED_HOST; preq.flags.pooled = false;
  auto r = rt.allocate(preq);
  // If no CUDA, the pinned domain is unavailable -> honest rejection.
  if (!r.ok()) { std::printf("  [skip] pinned host memory requires CUDA\n"); return; }
  BufferHandle p = std::move(r.value());
  auto s = rt.stats();
  CHECK_TRUE(s.ok());
  if (s.ok()) CHECK_EQ((long long)s.value().pinned_host_bytes, (long long)(1 << 20));
  CHECK_TRUE(p.verify().ok());
  p.release();
  auto s2 = rt.stats();
  if (s2.ok()) CHECK_EQ((long long)s2.value().pinned_host_bytes, 0);
}

int main() {
  cuda_roundtrip();
  pinned_capacity();
  return ubtest::report("cuda");
}