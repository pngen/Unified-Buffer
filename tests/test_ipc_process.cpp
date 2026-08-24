#include "unified_buffer/runtime.hpp"
#include "unified_buffer/buffer.hpp"
#include "unified_buffer/export.hpp"
#include "test_framework.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#endif

using namespace unified_buffer;

// Client mode: open the shared segment by name and verify every byte.
static int client_mode(const std::string& name, std::uint64_t size, unsigned char expect) {
  Runtime rt;
  ExportDescriptor d;
  d.format_version = kExportFormatVersion;
  d.size = size;
  d.domain = MemoryDomain::SHARED_HOST;
  d.handle_kind = "shared";
  d.handle = name;
  d.access = AccessMode::READ;
  auto imp = rt.import(d, kDefaultNamespace, "ipc-client");
  if (!imp.ok()) { std::printf("client: import failed: %s\n", imp.message().c_str()); return 1; }
  BufferHandle b = std::move(imp.value());
  auto m = b.map(AccessMode::READ);
  if (!m.ok()) { std::printf("client: map failed: %s\n", m.message().c_str()); return 1; }
  const unsigned char* p = static_cast<const unsigned char*>(m.value().data());
  bool ok = true;
  if (p[0] != expect || p[size - 1] != expect) ok = false;
  if (ok) { for (std::uint64_t i = 0; i < size; ++i) if (p[i] != expect) { ok = false; break; } }
  m.value().release();
  b.release();
  std::printf("client: %s\n", ok ? "verify PASS" : "verify FAIL");
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--client") {
    std::string name = argv[2];
    std::uint64_t size = std::strtoull(argv[3], nullptr, 10);
    unsigned char expect = static_cast<unsigned char>(std::strtoul(argv[4], nullptr, 10));
    return client_mode(name, size, expect);
  }

#if defined(_WIN32)
  // Server mode: create a shared segment, fill it, spawn this exe as the
  // importer, and verify the child reads identical bytes.
  const std::uint64_t size = 1 << 20;
  const unsigned char expect = 0x5A;
  Runtime rt;
  AllocationRequest req; req.size = size; req.domain = MemoryDomain::SHARED_HOST; req.flags.exportable = true; req.flags.zero_on_alloc = false;
  auto r = rt.allocate(req);
  CHECK_TRUE(r.ok());
  if (!r.ok()) return 1;
  BufferHandle b = std::move(r.value());
  auto m = b.map(AccessMode::READ_WRITE);
  CHECK_TRUE(m.ok());
  if (m.ok()) { unsigned char* p = static_cast<unsigned char*>(m.value().data()); for (std::uint64_t i = 0; i < size; ++i) p[i] = expect; m.value().release(); }
  auto d = rt.export_buffer(b);
  CHECK_TRUE(d.ok());
  if (!d.ok()) return 1;
  std::string name = d.value().handle;

  // Build the child command line.
  char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string cmd = std::string("\"") + exe + "\" --client " + name + " " + std::to_string(size) + " " + std::to_string((int)expect);
  STARTUPINFOA si{}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<char> cmdline(cmd.begin(), cmd.end()); cmdline.push_back('\0');
  if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    std::printf("server: CreateProcess failed\n");
    CHECK_TRUE(false);
    b.release();
    return 1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0; GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  std::printf("server: child exit=%d\n", (int)exit_code);
  CHECK_EQ((long long)exit_code, 0);
  b.release();
  return ubtest::report("ipc-process");
#else
  std::printf("  [skip] process IPC test requires Windows process APIs on this host\n");
  return 0;
#endif
}
