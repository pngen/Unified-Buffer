# Testing

This document describes the test layout in Unified Buffer 1.0.0, the harness contract, how failures map to exit codes, how
to run the suite, and the invariant list the tests check.

The test tree is under [`tests/`](../tests/):

```
tests/
├── CMakeLists.txt         # globs test_*.cpp into executables and registers them with CTest
├── test_framework.hpp     # minimal harness: CHECK macros, report(), code_name()
├── test_smoke.cpp         # host round-trip + RAII/baseline accounting
├── test_domains.cpp       # shared-memory export/import, file-backed reopen, pool reuse + cross-owner
│                          #   zeroing, quotas, views/bounds, leases, migration + copies
├── test_cuda.cpp          # CUDA device/pinned round-trip (host->pinned->device->pinned->host), device pointer
├── test_concurrency.cpp   # 8-thread alloc/free churn, read/write lease contention
├── test_failure_injection.cpp  # reservation rollback on capacity, migration failure preserves source
├── test_ipc_process.cpp   # real two-process shared-memory import/verify (spawns a child process)
├── test_property.cpp      # 6000-step random op sequence asserting invariants + baseline accounting
└── test_adversarial.cpp   # double-free, invalid requests, forged/oversized descriptors, path traversal,
                           #   cross-namespace import, write-of-read-only, bounds/overflow
```

## Test coverage

Each executable exercises a distinct slice of the runtime; together they enforce the invariant list at the bottom.

- **test_smoke** — basic host allocation, map, fill/read-back, checksum, verify(), release, and RAII with
  accounting returning to baseline (active_buffers/outstanding_allocations/host_bytes == 0).
- **test_domains** — SHARED_HOST export into a second Runtime and import verification; MMAP_STORAGE write → flush →
  release → reopen → verify; pool reuse (pool_hits increments) and cross-owner zeroing; namespace quota rejection with
  no leak; view bounds + invalid-slice rejection; read/write/exclusive lease conflict; and migration
  (HOST→DEVICE→HOST on CUDA hosts, else HOST→SHARED_HOST) with content preservation and stale-generation detection.
- **test_cuda** — when a CUDA device is present, a full host→pinned→device→pinned→host round-trip with exact-byte
  verification, device_pointer() borrowed access, and a post-cleanup check that device_bytes and
  outstanding_allocations are zero.
- **test_concurrency** — 8 threads × 2k allocation/free cycles, and concurrent lease acquisition with read/write
  conflict detection; after join, active_buffers and outstanding_allocations are zero.
- **test_failure_injection** — enforcing a small host cap and observing reserve rollback (no committed leak), and a
  migration into a capped target failing while leaving the source authoritative.
- **test_ipc_process** — a server process creates a SHARED_HOST segment, spawns a child process that imports it by name
  and verifies every byte, then waits for the child's exit code. This is process-level, not thread-level, validation.
- **test_property** — a seeded PRNG drives 6000 random allocate/free/map/view/lease/verify/migrate operations; after
  releasing everything and trimming the pool, every byte accounting counter returns to zero.
- **test_adversarial** — double release is a no-op; zero-size/non-power-of-two-alignment/overflow size are rejected;
  forged (wrong format version) and oversized export descriptors are rejected on import; a file import with a path
  traversal handle is rejected; cross-namespace import is refused; writing to a read-only buffer is denied; and
  out-of-bounds / overflow offsets are rejected.

---

## How the suite is built and run

[`tests/CMakeLists.txt`](../tests/CMakeLists.txt) discovers every `test_*.cpp` in the directory, builds each as a small
executable (linked against `unified_buffer`), and registers it with CTest under its file name. The suite is enabled with
`UNIFIED_BUFFER_BUILD_TESTS` (`ON` by default).

Run it with:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`ctest` runs each registered test and reports pass/fail. Add `-C Release` (as above) when using a multi-config generator so
the right configuration is executed.

---

## The harness contract

[`test_framework.hpp`](../tests/test_framework.hpp) is a self-contained, dependency-free harness. Each test executable
defines its own `main()` and uses the provided macros:

```cpp
CHECK(cond)          // increments checks(); increments failures() if cond is false
CHECK_EQ(a, b)       // compares a and b; reports on mismatch
CHECK_TRUE(cond)     // CHECK(cond)
CHECK_FALSE(cond)    // CHECK(!(cond))
```

- `ubtest::checks()` counts executed checks.
- `ubtest::failures()` counts failed checks.
- `ubtest::report(name)` prints `[PASS]` or `[FAIL]` with the counts and returns the exit code.
- `ubtest::ok(r)` returns `r.ok()` for a `Result`-like object.
- `ubtest::code_name(int)` maps an `ErrorCode` ordinal to its string name.

### Exit codes

`report` returns `0` on success (zero failures) and `1` on failure (one or more failed checks). Each test executable returns
that value to the OS, so:

- Exit code **0** — all checks passed.
- Exit code **1** — at least one check failed.

> Note: the `tests/CMakeLists.txt` comment says a test returns the *number* of failed checks, but the harness `report()`
> actually returns `0` or `1` (a pass/fail flag). The observable behavior is `0 = pass`, `1 = fail`.

---

## Current test targets

There is currently **one** compiled test target:

### `test_smoke`
The smoke/unit test in [`test_smoke.cpp`](../tests/test_smoke.cpp). It exercises the core host path:

- Allocates a 1 MiB `HOST` buffer (with zeroing, `pooled = false`).
- Asserts the handle is valid, the size is correct, and the domain is `HOST`.
- Maps it `READ_WRITE`, writes a byte pattern, and reads it back through a `view`.
- Asserts the CRC-32C `checksum()` is stable (deterministic).
- Asserts `verify()` succeeds.
- Releases the buffer and asserts the handle is no longer valid.
- Verifies RAII accounting: after a scoped host allocation, `active_buffers`, `outstanding_allocations`, and
  `host_bytes` return to baseline, and `total_frees` increments.

---

## Test categories

The harness is designed so a test can be added by dropping a new `<name>.cpp` into `tests/` that defines `main()` and
returns `ubtest::report(...)`. The intended categories — which the current suite is structured to grow into — are:

| Category | What it targets |
| --- | --- |
| Unit | Single-object behaviour: allocate/view/map/lease/copy/checksum/verify on one buffer. |
| Integration | Cross-cutting behaviour: migration between domains, export/import, pool reuse, namespaces, quotas. |
| Concurrency | Multi-threaded allocation/lease/copy and the locking and lease-conflict guarantees. |
| Failure-injection | Backend failure paths, quota/cap rejections, reservation-mismatch and early-return symmetry. |
| Property | Invariants that must hold for arbitrary sequences of operations (identity, generation, no-double-free). |
| Adversarial | Stale handle/generation, forged descriptors, malformed input, path traversal. |
| Backend | Per-domain behaviour: host, shared memory, file-backed, and (when compiled) pinned/device. |
| Multi-process | Interprocess sharing of `SHARED_HOST` and file-backed reopen across two processes. |

> **Status note.** Only `test_smoke` is present in 1.0.0. The categories above are the intended coverage; additional
> `test_*.cpp` files can be added following the same pattern and will be built and registered automatically by the glob.

Except for the shared/file and multi-process categories (which need the corresponding backend enabled), each test is a plain
executable and can be run directly under `ctest` or by invoking the built executable.

---

## How failures map to exit codes

A test executable returns its exit code from `main()`:

```cpp
int main() {
  test_host_roundtrip();
  test_raii_and_baseline();
  return ubtest::report("smoke");   // 0 = pass, 1 = fail
}
```

Because `report` returns `0` or `1`, the exit code is a pass/fail flag, not a count. This is what CTest observes. To diagnose
a failure, read the `[FAIL]` line (which names the test and the failed check, with source file and line) and rerun the single
test target directly.

---

## The invariant list

The tests verify the invariants documented in [DESIGN.md](DESIGN.md#invariants). The key ones are:

- **Identity:** a live buffer has a non-null `BufferId`, is in the registry once, and its id never changes.
- **Generation:** creation at `1`; any stale generation is rejected; migration bumps the generation exactly once.
- **Ownership:** a buffer has one, unambiguous `Ownership`; borrowed memory is never freed; no-double-free holds.
- **Capacity/reservation:** an allocation never exceeds a cap or quota; reservation/commit is symmetric on every path.
- **Leases:** `READ` coexist; `WRITE` and `EXCLUSIVE` exclude all others; migration is refused under a write/exclusive lease.
- **Mapping:** `map` of a `DEVICE` buffer fails; the last map of a `MAPPED` buffer returns it to `ALLOCATED`.
- **Pool:** a pooled slot only returns to a compatible key; pool size is bounded; cross-owner reuse is zeroed when required.
- **Migration:** destination is verified before the source is released; the buffer is `pool_eligible = false` afterwards.

See [DESIGN.md](DESIGN.md#invariants) for the full numbered list.

---

## Backend availability

The `HOST` and (when enabled) `SHARED_HOST` / `MMAP_STORAGE` domains are always-compiled on the validated Windows
platform. The `PINNED_HOST` and `DEVICE` domains are only present when the CUDA backend was compiled and a device is
available; tests that need them should be skipped, not failed, when `Runtime::domains()` reports them disabled. The
multi-process tests require the corresponding backend to be enabled and a process-spawn mechanism.