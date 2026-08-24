#pragma once
// Minimal self-contained test harness.  Each test executable defines its own
// main() and returns the number of failed checks.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

namespace ubtest {

inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }

template <class R> inline bool ok(const R& r) { return r.ok(); }
inline const char* code_name(int code) {
  switch (code) {
    case 0: return "ok";
    case 1: return "invalid_argument";
    case 2: return "out_of_capacity";
    case 3: return "quota_exceeded";
    case 4: return "unsupported_domain";
    case 5: return "unsupported_capability";
    case 6: return "stale_handle";
    case 7: return "stale_generation";
    case 8: return "lease_conflict";
    case 9: return "mapping_failure";
    case 10: return "backend_failure";
    case 11: return "device_unavailable";
    case 12: return "invalid_device";
    case 13: return "alignment_error";
    case 14: return "bounds_error";
    case 15: return "integrity_failure";
    case 16: return "import_failure";
    case 17: return "export_failure";
    case 18: return "permission_failure";
    case 19: return "timeout";
    case 20: return "resource_busy";
    case 21: return "lifecycle_violation";
    case 22: return "overflow";
    case 23: return "not_found";
    case 24: return "already_exists";
    case 25: return "closed";
    case 26: return "state_invalid";
    default: return "unknown";
  }
}

inline int report(const char* name) {
  if (failures() == 0) {
    std::printf("[PASS] %s (%d checks)\n", name, checks());
    return 0;
  }
  std::printf("[FAIL] %s (%d failures of %d checks)\n", name, failures(), checks());
  return 1;
}

}  // namespace ubtest

#define CHECK(cond) do { ++ubtest::checks(); if (!(cond)) { ++ubtest::failures(); std::printf("  FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ(a, b) do { ++ubtest::checks(); auto _va = (a); auto _vb = (b); if (!(_va == _vb)) { ++ubtest::failures(); std::printf("  FAIL at %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)_va, (long long)_vb); } } while (0)

#define CHECK_FALSE(a) CHECK(!(a))
#define CHECK_TRUE(a) CHECK(a)