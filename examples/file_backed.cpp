#include "common.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
using namespace unified_buffer;

int main() {
  Runtime producer;
  if (!ubex::domain_enabled(producer, MemoryDomain::MMAP_STORAGE)) {
    ubex::print("[file_backed] SKIP: MMAP_STORAGE domain unavailable");
    return 0;
  }

  const std::uint64_t n = 4096;
  AllocationRequest req;
  req.size = n;
  req.domain = MemoryDomain::MMAP_STORAGE;
  req.flags.exportable = true;         // required for export_buffer -> handle path
  req.flags.pooled = false;
  req.flags.zero_on_alloc = true;

  auto ar = producer.allocate(req);
  if (!ar.ok()) { ubex::print("[file_backed] allocate failed: " + ubex::errstr(ar)); return 1; }
  BufferHandle h = std::move(ar.value());

  auto m = h.map(AccessMode::READ_WRITE);
  if (!m.ok()) { ubex::print("[file_backed] map failed: " + ubex::errstr(m)); return 1; }
  unsigned char* p = static_cast<unsigned char*>(m.value().data());
  for (std::uint64_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(i * 3 + 1);
  m.value().release();

  auto ex = producer.export_buffer(h);
  if (!ex.ok()) { ubex::print("[file_backed] export failed: " + ubex::errstr(ex)); return 1; }
  const std::string path = ex.value().handle;
  ubex::print("[file_backed] exported handle path = " + path);

  // The library exposes no public BufferHandle::flush(); dirty mmap pages are
  // written back to the backing file when the view/file handle is closed, so
  // releasing the producer handle below persists the bytes to disk.
  h.release();

  // Reopen the file-backed segment in a brand-new Runtime by importing the
  // descriptor derived from the exported handle path.
  Runtime consumer;
  ExportDescriptor d;
  d.format_version = kExportFormatVersion;
  d.buffer_id = BufferId{};                 // re-import creates a fresh identity
  d.generation = 1;
  d.ns = kDefaultNamespace;
  d.backend = BackendId::FILE_BACKED;
  d.domain = MemoryDomain::MMAP_STORAGE;
  d.size = n;
  d.alignment = 64;
  d.access = AccessMode::READ_WRITE;
  d.handle_kind = "file";
  d.handle = path;
  d.exportable = true;
  d.writable = true;
  d.label = "reopened-file";

  auto im = consumer.import(d, kDefaultNamespace, "reopen");
  if (!im.ok()) { ubex::print("[file_backed] reopen import failed: " + ubex::errstr(im)); return 1; }
  BufferHandle h2 = std::move(im.value());
  auto m2 = h2.map(AccessMode::READ);
  if (!m2.ok()) { ubex::print("[file_backed] reopened map failed: " + ubex::errstr(m2)); return 1; }
  const unsigned char* q = static_cast<const unsigned char*>(m2.value().data());
  bool same = true;
  for (std::uint64_t i = 0; i < n; ++i) if (q[i] != static_cast<unsigned char>(i * 3 + 1)) { same = false; break; }
  m2.value().release();
  ubex::print(std::string("[file_backed] reopened content matches = ") + (same ? "yes" : "no"));
  if (!same) return 1;

  h2.release();

  // Clean up the temp file once nothing holds it open any longer.
  if (!path.empty()) {
    if (std::remove(path.c_str()) == 0) {
      ubex::print("[file_backed] deleted temp file");
    } else {
      ubex::print("[file_backed] note: could not delete temp file (still open?)");
    }
  }
  ubex::print("[file_backed] PASS");
  return 0;
}
