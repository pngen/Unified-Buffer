# Unified Buffer

Unified Buffer is an open-source, vendor-neutral runtime for governing allocation, ownership, mapping, sharing, pooling, reuse, residency, and lifecycle across heterogeneous memory domains.

It is **not** merely an allocator, **not** a cache, and it does **not** claim universal zero-copy or transparent coherence. It is the abstraction layer that higher-level systems use to reason about a **single, consistent memory object** no matter which memory domain backs it. It gives every buffer a stable 128-bit identity, a generation, an ownership model, and a lifecycle — independent of the page that holds the bytes.

This document is the entry point. For the rest of the documentation set, see [docs/](docs/) and the file list at the bottom.

---

## Table of contents

- [What It Is](#what-it-is)
- [What It Is Not](#what-it-is-not)
- [Quick Start](#quick-start)
- [Core concepts](#core-concepts)
- [Runnable examples](#runnable-examples)
- [Public API surface](#public-api-surface)
- [Documentation set](#documentation-set)
- [License](#license)

---

## What It Is

Unified Buffer is a **runtime library** (a static C++20 library, `libunified_buffer`) that owns the *metadata* and *policy* around memory objects. It does not replace a platform allocator or driver; it *coordinates* them through a small backend abstraction.

Concretely, a buffer in Unified Buffer:

- Has a **stable identity** (`BufferId`, 128-bit) that never changes even when the address changes.
- Has a **generation** (`BufferGeneration`, 64-bit) that advances whenever the buffer's authoritative binding or residency changes.
- Lives in exactly one **memory domain** at a time — `HOST`, `PINNED_HOST`, `DEVICE`, `SHARED_HOST`, or `MMAP_STORAGE`.
- Is governed by an explicit **ownership** policy — `RUNTIME`, `BORROWED`, `ADOPTED`, `IMPORTED`, or `SHARED`.
- Decays through a **state machine** (`DECLARED` … `INVALID`) with a fixed table of legal transitions.
- Can be **pooled** (reused across allocations of a compatible domain/size-class/alignment) and **zeroed** according to a configurable `ZeroingPolicy`.
- Can be **mapped** for CPU access, **viewed** as a non-owning slice, **leased** (read/write/exclusive) to coordinate access, and **verified** / **checksummed** for integrity.
- Can be **exported** to a versioned descriptor and **imported** across a library/process boundary — for shared memory and file-backed domains this is a real cross-process handle; for host/device it is same-runtime re-share only.
- Can be **migrated** between domains, with a copy and a verified swap of the single backing authority.

The runtime also provides per-domain capacity caps, per-namespace quotas, accounting and telemetry, and a bounded reuse pool. It reports capabilities explicitly and **rejects** unsupported operations rather than silently emulating them.

## What It Is Not

Unified Buffer draws a hard line around the scope of what it *does*. The following are **not** Unified Buffer. Several of them are systems that use Unified Buffer as their consistent memory object; the rest are adjacent but distinct.

- **Not a FlashTier** — it does not tier hot data to SSD/NVMe or decide what should be cold.
- **Not a Tensor Cache** — it does not cache model weights or KV tensors, and it does not manage cache eviction of model data.
- **Not a KV Fabric** — it is not a distributed key/value or memory fabric.
- **Not a tensor framework** — it knows nothing about tensor shapes, dtypes, strides, or operators.
- **Not a garbage collector** — it never traces, marks, or reclaims reachable objects; release is explicit and deterministic.
- **Not a distributed storage engine** — it does not replicate, shard, or place data across nodes.
- **Not a transfer scheduler** — it does not schedule async transfers, enqueue kernels, or manage streams.
- **Not an inference server** — it has no model runtime, batching, or serving loop.
- **Not a replacement for CUDA / HIP / Level Zero** — it does not replace a driver or runtime API; it *uses* the active device backend (CUDA when present) through its own `IDomain` abstraction.
- **Not a coherence protocol** — it does not automatically keep heterogeneous views coherent, and it does not promise universal zero-copy. `COHERENT` is a *capability flag* that a domain advertises when its memory is CPU-coherent; it is not a promise of cross-device coherence management.
- **Not a daemon** — there is no control plane, no socket, no background service, and no control protocol to talk to.

Unified Buffer is the **abstraction layer** those systems use when they need a consistent memory object whose identity, ownership, residency, and lifecycle are well defined.

## Quick Start

### Windows (the validated platform for this release)

From a **Developer Command Prompt for Visual Studio** (or any terminal with `cmake` and the MSVC toolchain on the `PATH`), in the repository root:

```cmake
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This configures, builds, and runs the CTest suite. The first command creates the `build/` directory as an x64 Visual Studio 2022 solution with a `Release` configuration; the second compiles it; the third runs registered tests.

### Linux (intent; not the validated release target)

The project is CMake-driven so it can be configured on Linux with a generator of your choice. The checked-in source uses MSVC-specific calls for the host domain (`_aligned_malloc`/`_aligned_free`) and provides Windows implementations of the shared-memory and file-backed domains, so the **validated platform for this release is Windows**. Linux commands are shown for intent and for CI that can supply an equivalent toolchain, but capabilities there are limited to the host domain (and the CUDA backend, separately, if compiled):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Build options

The following CMake options are available (see [CMakeLists.txt](CMakeLists.txt)):

| Option | Default | Meaning |
| --- | --- | --- |
| `UNIFIED_BUFFER_ENABLE_CUDA` | `ON` | Probe for the CUDA Toolkit and enable device/pinned domains when found. |
| `UNIFIED_BUFFER_BUILD_TESTS` | `ON` | Build the CTest test targets. |
| `UNIFIED_BUFFER_BUILD_EXAMPLES` | `ON` | Build example targets. |
| `UNIFIED_BUFFER_BUILD_BENCHMARKS` | `ON` | Build benchmark targets. |
| `UNIFIED_BUFFER_WARNINGS_AS_ERRORS` | `ON` | Promote warnings to errors (`/WX` on MSVC, `-Werror` otherwise). |

The library target is `unified_buffer` and is also exposed as the alias `UnifiedBuffer::unified_buffer`. You can link it with:

```cmake
target_link_libraries(your_target PRIVATE UnifiedBuffer::unified_buffer)
```

A tiny CLI, `unified-buffer`, prints the runtime version string.

## Core concepts

### Identity and generation

A buffer is identified by a `BufferId` (two 64-bit halves, 128 bits total). The address of a buffer may change; the identity must not. A `BufferGeneration` (64-bit) starts at `1` and advances on operations that invalidate earlier handles or authoritative assumptions (currently, **migration**). Operations that carry both an id and an expected generation are rejected as `stale_generation` when they disagree, and operations that reference a finalized or unknown id are rejected as `stale_handle`.

### Memory domains

A **memory domain** is the logical kind of memory a buffer lives in. There are five in 1.0.0:

| Domain | Meaning |
| --- | --- |
| `HOST` | Standard aligned host memory (pageable, CPU-addressable). |
| `PINNED_HOST` | CUDA pinned host memory (CPU-addressable, DMA-capable). |
| `DEVICE` | CUDA device memory (not CPU-addressable in general). |
| `SHARED_HOST` | Interprocess shared memory (Windows file-mapping objects; `/dev/shm` on Linux). |
| `MMAP_STORAGE` | File-backed memory-mapped storage (memory-mapped files). |

See [docs/MEMORY_DOMAINS.md](docs/MEMORY_DOMAINS.md).

### Backends and capabilities

Each domain is backed by an `IDomain` implementation (a **backend**). Backends advertises a `Capabilities` set; the runtime rejects an operation that the backend does not advertise rather than emulating it. Backend ids are `HOST_MALLOC`, `CUDA`, `WIN_SHARED_FILE`, and `FILE_BACKED`.

### Ownership

Every buffer has an explicit ownership mode that determines whether the runtime may free it:

| Mode | Runtime frees it? |
| --- | --- |
| `RUNTIME` | Yes — this is a runtime-owned allocation. |
| `BORROWED` | **Never.** The pointer belongs to the caller. |
| `ADOPTED` | Yes — the runtime takes over deallocation. |
| `IMPORTED` | Yes — the runtime releases the imported handle/import. |
| `SHARED` | Yes — via the external handle wrapper. |

See [docs/OWNERSHIP.md](docs/OWNERSHIP.md).

### Lifecycle

Buffers transition through the `BufferState` machine under a fixed, documented table. See [docs/LIFECYCLE.md](docs/LIFECYCLE.md).

### Pooling and zeroing

Allocations may be serviced from a bounded reuse pool keyed by domain/backend/device/size-class/alignment/exportability. When a buffer is returned to the pool, its backing is retained rather than freed, subject to `max_idle_bytes` and `max_objects`. Reuse honors the namespace policy or an explicit per-request override through `ZeroingPolicy`. See [docs/DESIGN.md](docs/DESIGN.md) for the zeroing semantics.

## Runnable examples

All of the following use the real public API. They are written as standalone C++ fragments; include `<unified_buffer/runtime.hpp>` and link `UnifiedBuffer::unified_buffer`.

### Host allocation, map, and view

```cpp
#include <unified_buffer/runtime.hpp>
#include <cstdint>
#include <cstring>
using namespace unified_buffer;

Runtime rt;
AllocationRequest req;
req.size = 1 << 20;             // 1 MiB
req.domain = MemoryDomain::HOST;
req.flags.zero_on_alloc = true;
auto r = rt.allocate(req);      // Result<BufferHandle>
if (!r.ok()) return;
BufferHandle h = std::move(r.value());

auto m = h.map(AccessMode::READ_WRITE);
if (m.ok()) {
  auto* p = static_cast<uint8_t*>(m.value().data());
  std::memset(p, 0xAB, h.size());
  m.value().release();
}

auto v = h.view(0, h.size(), AccessMode::READ);
if (v.ok()) {
  const auto* p = static_cast<const uint8_t*>(v.value().data());
  (void)p;   // read through the view
  v.value().release();
}
// h falls out of scope -> released (RAII)
```

### Copy in and out

```cpp
Runtime rt;
AllocationRequest req{};
req.size = 4096;
req.domain = MemoryDomain::HOST;
auto h = rt.allocate(req).value();

uint8_t src[4096] = {};
h.copy_from(src, 0, sizeof(src));            // host -> buffer
uint8_t dst[4096] = {};
h.copy_to(dst, 0, sizeof(dst));              // buffer -> host
```

### Pool reuse

```cpp
Runtime rt;                   // pool enabled by default (RuntimeConfig)
{
  AllocationRequest req{};
  req.size = 1 << 20;         // enters pool-eligible size class
  req.domain = MemoryDomain::HOST;
  req.flags.pooled = true;
  auto h = rt.allocate(req).value();
}                             // released -> returned to pool (not freed)

AllocationRequest req2{};
req2.size = 1 << 20;
req2.domain = MemoryDomain::HOST;
req2.flags.pooled = true;
auto h2 = rt.allocate(req2).value();   // likely a pool hit
```

### Migration

```cpp
Runtime rt;
AllocationRequest req{};
req.size = 1 << 20;
req.domain = MemoryDomain::HOST;
auto h = rt.allocate(req).value();

auto migrated = rt.migrate(h, MemoryDomain::SHARED_HOST);
if (!migrated.ok()) return;
BufferHandle mh = std::move(migrated.value());   // new generation
```

### Shared-memory export / import

```cpp
Runtime rt;
AllocationRequest req{};
req.size = 1 << 20;
req.domain = MemoryDomain::SHARED_HOST;   // exportable domain
req.flags.exportable = true;
auto h = rt.allocate(req).value();

auto desc = rt.export_buffer(h);          // ExportDescriptor (format v1)
if (!desc.ok()) return;
auto d = desc.value();

Runtime rt2;
auto imported = rt2.import(d);            // Result<BufferHandle>
if (!imported.ok()) return;
BufferHandle ih = std::move(imported.value());
```

Host/device buffers export a **same-process** descriptor only; cross-process exchange of host or device buffers is not supported in 1.0.0 (use `SHARED_HOST`). See [docs/INTEROP.md](docs/INTEROP.md).

## Public API surface

The complete public API is defined by the headers under `include/unified_buffer/`:

- `runtime.hpp` — `Runtime`, `AllocationRequest`, `ExternalMemoryDesc`, `ExternalOwnership`.
- `buffer.hpp` — `BufferHandle`, `BufferLease`, `BufferMap`, `BufferView`, `LeaseKind`.
- `export.hpp` — `ExportDescriptor`, `kExportFormatVersion`.
- `telemetry.hpp` — `BackendInfo`, `DeviceInfo`, `DomainInfo`, `RuntimeStats`.
- `config.hpp` — `RuntimeConfig`, `PoolConfig`, `NamespaceConfig`.
- `core/types.hpp` — `MemoryDomain`, `BackendId`, `DeviceId`, `AccessMode`, `BufferState`, `Ownership`, `AllocationFlags`, `ZeroingPolicy`.
- `core/identity.hpp` — `BufferId`, `BufferGeneration`, `NamespaceId`, `kDefaultNamespace`.
- `core/error.hpp` — `ErrorCode`, `Error`.
- `core/result.hpp` — `Result<T>`, `Status`.
- `core/capabilities.hpp` — `Capability`, `Capabilities`.
- `core/checked_math.hpp` — overflow-safe helpers.
- `core/integrity.hpp` — `crc32c`, `crc32c_self_test`.
- `core/version.hpp` — `version_string`.
- `backends/domain.hpp` — `IDomain`, `NativeAllocation`, `CapacityInfo` (backend interface).
- `backends/*.hpp` — concrete backends (`HostDomain`, `CudaDeviceDomain`, `CudaPinnedDomain`, `SharedMemoryDomain`, `FileBackedDomain`).

## Documentation set

| Document | Contents |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Repo layout, layers, the `IDomain` abstraction, registry, control block, lifecycle, locking, per-domain accounting, pool, and the boundary against adjacent systems. |
| [docs/DESIGN.md](docs/DESIGN.md) | Object model (BufferId/generation/ownership), `Result`/`ErrorCode`, checked arithmetic, identity vs address, single-authority residency, invariants. |
| [docs/MEMORY_DOMAINS.md](docs/MEMORY_DOMAINS.md) | Each domain, its backend, capabilities, alignment, capacity semantics, and how to enable/configure it. |
| [docs/OWNERSHIP.md](docs/OWNERSHIP.md) | Ownership semantics, external memory wrapping, import ownership, no-double-free, borrowed-never-freed. |
| [docs/LIFECYCLE.md](docs/LIFECYCLE.md) | The `BufferState` machine, legal transitions, deterministic finalization, shutdown, stale-handle/generation rules. |
| [docs/INTEROP.md](docs/INTEROP.md) | Export/import descriptor (v1), what crosses the boundary, same-process host/device restrictions, interprocess sharing, file-backed reopen, DLPack/framework guidance. |
| [docs/SECURITY.md](docs/SECURITY.md) | Adversarial model and the protections that exist (and the non-crypto integrity guarantees). |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | Benchmark categories and how to measure them. |
| [docs/TESTING.md](docs/TESTING.md) | Test layout, mapping to exit codes, how to run, invariants. |
| [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) | Build, test, conventions, review process. |

## License

Apache License 2.0. Copyright (c) Summon Software Labs. See [LICENSE](LICENSE).
