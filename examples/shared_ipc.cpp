#include "common.hpp"
#include <cstdint>
#include <cstring>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime producer;
  Runtime consumer;

  if (!ubex::domain_enabled(producer, MemoryDomain::SHARED_HOST)) {
    ubex::print("[shared_ipc] SKIP: SHARED_HOST domain unavailable");
    return 0;
  }

  const std::uint64_t n = 4096;
  AllocationRequest req;
  req.size = n;
  req.domain = MemoryDomain::SHARED_HOST;
  req.flags.exportable = true;         // required for export_buffer
  req.flags.pooled = false;
  req.flags.zero_on_alloc = true;
  req.label = "shared-payload";

  auto ar = producer.allocate(req);
  if (!ar.ok()) { ubex::print("[shared_ipc] allocate failed: " + ubex::errstr(ar)); return 1; }
  BufferHandle h = std::move(ar.value());

  auto m = h.map(AccessMode::READ_WRITE);
  if (!m.ok()) { ubex::print("[shared_ipc] map failed: " + ubex::errstr(m)); return 1; }
  unsigned char* p = static_cast<unsigned char*>(m.value().data());
  for (std::uint64_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(i % 251);
  m.value().release();

  auto ex = producer.export_buffer(h);
  if (!ex.ok()) { ubex::print("[shared_ipc] export failed: " + ubex::errstr(ex)); return 1; }
  ubex::print("[shared_ipc] exported descriptor: handle_kind=" + ex.value().handle_kind +
              " format_version=" + std::to_string(ex.value().format_version));

  // Import into a separate Runtime instance (same process here; the named
  // mapping is what a real multi-process attach would also resolve).
  auto im = consumer.import(ex.value(), kDefaultNamespace, "shared-import");
  if (!im.ok()) { ubex::print("[shared_ipc] import failed: " + ubex::errstr(im)); return 1; }
  BufferHandle h2 = std::move(im.value());
  ubex::print("[shared_ipc] imported handle domain=" + std::string(to_string(h2.domain())) +
              " size=" + std::to_string(h2.size()));

  auto m2 = h2.map(AccessMode::READ);
  if (!m2.ok()) { ubex::print("[shared_ipc] imported map failed: " + ubex::errstr(m2)); return 1; }
  const unsigned char* q = static_cast<const unsigned char*>(m2.value().data());
  bool same = true;
  for (std::uint64_t i = 0; i < n; ++i) if (q[i] != static_cast<unsigned char>(i % 251)) { same = false; break; }
  m2.value().release();
  ubex::print(std::string("[shared_ipc] imported buffer reads the same bytes = ") + (same ? "yes" : "no"));
  if (!same) return 1;

  ubex::print("[shared_ipc] released a shared-memory handle (import shares the same segment: "
              "producer id=" + std::to_string(h.id().lo) + " consumer id=" + std::to_string(h2.id().lo) +
              ").  A real multi-process run would attach the same named segment.");

  h.release();
  h2.release();
  ubex::print("[shared_ipc] PASS");
  return 0;
}
