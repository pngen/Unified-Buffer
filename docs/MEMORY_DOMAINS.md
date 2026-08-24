# Memory Domains

Unified Buffer 1.0.0 defines five memory domains. Each domain is backed by a concrete `IDomain` implementation (a backend), advertises a set of `Capabilities`, and has a capacity and alignment model. This document describes each domain, its backend, capability set, alignment, capacity semantics, and how to enable or configure it.

The public enums are in [core/types.hpp](../include/unified_buffer/core/types.hpp) (`MemoryDomain`, `BackendId`, `DeviceId`) and [core/capabilities.hpp](../include/unified_buffer/core/capabilities.hpp).

---

## Overview

| `MemoryDomain` | Meaning | `BackendId` |
| --- | --- | --- |
| `HOST` | Standard aligned host memory (pageable, CPU-addressable). | `HOST_MALLOC` |
| `PINNED_HOST` | CUDA pinned host memory (CPU-addressable, DMA-capable). | `CUDA` |
| `DEVICE` | CUDA device memory (not CPU-addressable in general). | `CUDA` |
| `SHARED_HOST` | Interprocess shared memory (Windows file-mapping objects; `/dev/shm` on Linux). | `WIN_SHARED_FILE` |
| `MMAP_STORAGE` | File-backed memory-mapped storage (memory-mapped files). | `FILE_BACKED` |

---

## Domain enablement

**Host, shared, and file** domains are enabled through `RuntimeConfig`:

```cpp
struct RuntimeConfig {
  std::string name = "unified_buffer";
  bool enable_host   = true;
  bool enable_shared = true;
  bool enable_file   = true;
  bool enable_pool   = true;
  PoolConfig pool;
  std::vector<NamespaceConfig> namespaces;
  std::uint64_t host_capacity_hint = 0;   // 0 = auto-detect host capacity
  // Per-domain capacity caps (0 = backend-reported / default):
  std::uint64_t host_cap   = 0;
  std::uint64_t pinned_cap = 0;
  std::uint64_t device_cap = 0;
  std::uint64_t shared_cap = 0;
  std::uint64_t file_cap   = 0;
};
```

- `enable_host`, `enable_shared`, `enable_file` toggle the `HOST`, `SHARED_HOST`, and `MMAP_STORAGE` domains. All default to `true`.
- `enable_pool` toggles the reuse pool (see [Pooling and capacity](#pooling-and-capacity)).
- `host_capacity_hint` supplies a host capacity when you do not want auto-detection (which reads physical memory on Windows).
- The per-domain caps (`host_cap`, `pinned_cap`, `device_cap`, `shared_cap`, `file_cap`) set the hard ceiling for the domain. A value of `0` means "use the backend-reported/default capacity". When both a configured cap and a backend total exist, the runtime uses `min(configured, backend_total)`.

**The `PINNED_HOST` and `DEVICE` domains are CUDA-backed and optional.** They are not toggled by `RuntimeConfig`; they are created only when:

1. The CUDA backend was compiled in (the CMake option `UNIFIED_BUFFER_ENABLE_CUDA` is `ON` and the CUDA Toolkit was found), and
2. `cuda_support::available()` reports at least one functioning CUDA device at runtime.

If those conditions are not met, `domain_enabled(PINNED_HOST)` and `domain_enabled(DEVICE)` are `false`, and any attempt to allocate in them returns `ErrorCode::unsupported_domain`.

> **Validated platform note.** The checked-in source uses MSVC-specific host allocation (`_aligned_malloc` and `_aligned_free`) and Windows implementations of the shared and file backends. The validated platform for this release is **Windows**. On a non-Windows host, the shared and file backends are compiled but their allocate/import methods return `unsupported_domain`, so only the host domain (and the separately compiled CUDA domains) are functional.

---

## `HOST` — standard host memory

**Backend:** `HostDomain` (`BackendId::HOST_MALLOC`).

- **What it is.** Pageable, CPU-addressable host memory, allocated with `_aligned_malloc` and freed with `_aligned_free`.
- **Capabilities.** `HOST_READABLE`, `HOST_WRITABLE`, `CPU_MAPPABLE`, `PAGEABLE`, `EXPORTABLE`, `IMPORTABLE`, `PERSISTENT`, `COHERENT`, `SUPPORTS_SUBALLOCATION`.
- **Alignment.** `default_alignment()` returns `sizeof(void*)`; `preferred_alignment()` returns `64`. The runtime caps requested alignment at `4096` and requires a power of two. `_aligned_malloc` requires a power-of-two alignment at least `sizeof(void*)`.
- **Capacity.** `capacity()` returns `total_capacity` equal to `host_capacity_hint` if set, otherwise the system physical memory (as reported by `GlobalMemoryStatusEx` on Windows), with a fallback of `1 TiB` if detection yields zero. `host_cap` overrides/maxes this via `capacity_for`.
- **Zeroing.** Single pass `std::memset` over the requested range.
- **CPU map.** `map()` returns the same address as the allocation (the allocation is already CPU-addressable); no separate mapping object is created.
- **Enable/configure.** `RuntimeConfig::enable_host` (`true` by default); `host_capacity_hint`; `host_cap`.

---

## `PINNED_HOST` — CUDA pinned host memory

**Backend:** `CudaPinnedDomain` (`BackendId::CUDA`). Optional.

- **What it is.** Host memory pinned for DMA, allocated with `cudaHostAlloc` (`cudaHostAllocDefault`) and freed with `cudaFreeHost`. This avoids pageable-host staging penalties for device copies.
- **Capabilities.** `HOST_READABLE`, `HOST_WRITABLE`, `CPU_MAPPABLE`, `PINNED`, `DIRECTLY_DMA_CAPABLE`, `ASYNC_COPYABLE`.
- **Alignment.** `preferred_alignment()` and `default_alignment()` return `256` (the runtime `validate_request` uses `preferred_alignment` for an unset alignment; if a request passes `alignment == 0` here it becomes `256`).
- **Capacity.** `capacity()` reports the device `cudaMemGetInfo` total, used as the pinned total when `pinned_cap` is `0`.
- **Zeroing.** `std::memset` over the CPU alias.
- **CPU map.** `map()` returns the pinned host pointer.
- **Enable/configure.** Present only when CUDA is compiled and a device is available at runtime. There is no `RuntimeConfig` toggle; configure the ceiling with `pinned_cap`.

> **Device caveat.** The pinned domain is tied to the CUDA runtime; it is not available when the CUDA backend is absent.

---

## `DEVICE` — CUDA device memory

**Backend:** `CudaDeviceDomain` (`BackendId::CUDA`). Optional.

- **What it is.** Device memory, allocated with `cudaMalloc` and freed with `cudaFree`. Not CPU-addressable in general.
- **Capabilities.** `DIRECTLY_DMA_CAPABLE`, `HOST_READABLE`, `HOST_WRITABLE`, `DEVICE_READABLE`, `DEVICE_WRITABLE`, `ASYNC_COPYABLE`, `SUPPORTS_EXTERNAL_HANDLE`.
- **Alignment.** `preferred_alignment()` and `default_alignment()` return `256`. The runtime rejects a `DEVICE` request whose alignment exceeds `4096`.
- **Capacity.** `capacity()` reports `total_capacity` and `free` from `cudaMemGetInfo`.
- **CPU map.** `map()` fails with `unsupported_capability` — device memory is not CPU-mappable. `checksum()` of device content also fails (it requires a host round-trip).
- **Device pointer access.** `BufferHandle::device_pointer()` returns the raw device pointer (borrowed, tied to the handle and generation). It is the consumer's obligation to use it only while the handle is alive and the generation matches.
- **Cross-domain copy.** Copying into or out of device memory goes through `copy_memory`, which uses `cudaMemcpy` (`DeviceToDevice`, `HostToDevice`, or `DeviceToHost`) and then `cudaDeviceSynchronize()`.
- **Zeroing.** `cudaMemset` followed by `cudaDeviceSynchronize()`.
- **Enable/configure.** Present only when CUDA is compiled and a device is available. A `DEVICE` allocation requires a valid `DeviceId` with a non-negative `index`; otherwise `invalid_device`. The runtime's migration to `DEVICE` uses the active device index (the first available device at startup). Configure the ceiling with `device_cap`.
- **No CUDA IPC in 1.0.0.** There is no `cudaIpc`-based export/import. Device buffers export a *same-process, same-runtime* (local) descriptor only; they cannot be shared across processes.

> **Migrating to `DEVICE`.** `Runtime::migrate(h, MemoryDomain::DEVICE)` copies the host buffer to the device and returns a new handle whose generation has advanced. The old host backing is freed.

---

## `SHARED_HOST` — interprocess shared memory

**Backend:** `SharedMemoryDomain` (`BackendId::WIN_SHARED_FILE`).

- **What it is.** Memory shared across processes. On Windows it uses **file-mapping objects** (`CreateFileMappingA` plus `MapViewOfFile`) with a unique, generated name (for example `UnifiedBuffer_<seed>_<counter>`). On Linux it is intended to use `/dev/shm`; the checked-in non-Windows branch is a stub that returns `unsupported_domain`.
- **Capabilities.** `HOST_READABLE`, `HOST_WRITABLE`, `CPU_MAPPABLE`, `EXPORTABLE`, `IMPORTABLE`, `SHAREABLE_ACROSS_PROCESS`, `COHERENT`.
- **Alignment.** `preferred_alignment()` returns `4096`; `default_alignment()` returns `64`.
- **Capacity.** `capacity()` returns `total_capacity` equal to the configured `shared_cap`. A `shared_cap` of `0` means the backend reports no hard total (so the domain behaves as configured from `capacity_for`).
- **Export/import.** This is the primary cross-process domain. `export_handle` returns the mapping **name**; `import_handle` opens that name with `OpenFileMappingA` and maps it. See [INTEROP.md](INTEROP.md). The import rejects names containing a path separator (backslash or slash) or longer than 512 characters.
- **Zeroing.** `std::memset` over the mapped view.
- **CPU map.** `map()` returns the shared view.
- **Enable/configure.** `RuntimeConfig::enable_shared` (`true` by default); ceiling with `shared_cap`.

---

## `MMAP_STORAGE` — file-backed mapped storage

**Backend:** `FileBackedDomain` (`BackendId::FILE_BACKED`).

- **What it is.** Memory-mapped files. Backed by a **file** created/mapped in a base directory. On Windows it uses `CreateFileA` plus `CreateFileMappingA` plus `MapViewOfFile`; on Linux it is intended to use memory-mapped files via `mmap`, but the checked-in non-Windows branch is a stub that returns `unsupported_domain`.
- **Capabilities.** `HOST_READABLE`, `HOST_WRITABLE`, `CPU_MAPPABLE`, `EXPORTABLE`, `IMPORTABLE`, `PERSISTENT`, `SUPPORTS_EXPLICIT_FLUSH`, `SUPPORTS_EXPLICIT_INVALIDATE`.
- **Alignment.** `preferred_alignment()` returns `4096`; `default_alignment()` returns `64`.
- **Base directory.** The `FileBackedDomain` constructor takes an optional `base_dir`. If empty, it resolves to the system temp directory (`GetTempPathA` on Windows, `TMPDIR` or `/tmp` on POSIX). The runtime's `attach_shared_and_file_backends` constructs the domain with `cfg.file_cap` and an *empty* base dir, so the default temp path is used.
- **Capacity.** `capacity()` returns `total_capacity` equal to the configured `file_cap`.
- **Export/import.** `export_handle` returns the file **path**; `import_handle` opens that path and maps it. File import enforces path-traversal protection: it rejects a path containing `..` or a backslash-based traversal segment, rejects paths longer than 1024 characters, and requires the path to be **beneath** the configured base directory (`path_is_beneath`). See [SECURITY.md](SECURITY.md) and [INTEROP.md](INTEROP.md).
- **Zeroing.** `std::memset` over the mapped view.
- **Flush.** `flush()` calls `FlushViewOfFile` and `FlushFileBuffers` on Windows.
- **CPU map.** `map()` returns the mapped view.
- **Enable/configure.** `RuntimeConfig::enable_file` (`true` by default); ceiling with `file_cap`. The base directory is not currently configurable from `RuntimeConfig`; it is the system temp directory.

---

## Per-domain capacity caps

The runtime enforces a hard ceiling per domain (see [Domain enablement](#domain-enablement) above). For a request to allocate in a domain:

1. `capacity_for(domain)` returns `min(configured_cap, backend_total)` when both are nonzero, otherwise whichever is nonzero, otherwise `0`.
2. `reserve_amount` adds the class size to the domain's reserved plus committed and rejects with `out_of_capacity` if that exceeds the cap.
3. A namespace quota (if set) is also checked; exceeding it returns `quota_exceeded`.

The cap applies per domain across all namespaces; the quota applies per (namespace, domain).

## Namespace quotas

Each namespace can impose per-domain byte quotas and an allocation-count quota:

```cpp
struct NamespaceConfig {
  std::string name;
  std::uint64_t host_quota   = 0;   // 0 = unlimited
  std::uint64_t pinned_quota = 0;
  std::uint64_t device_quota = 0;
  std::uint64_t shared_quota = 0;
  std::uint64_t file_quota   = 0;
  std::uint64_t allocation_count_quota = 0;
  ZeroingPolicy zeroing = ZeroingPolicy::ON_CROSS_OWNER_REUSE;
  bool allow_export = true;
  bool allow_import = true;
  int priority = 0;
};
```

- A quota value of `0` means *unlimited* for that domain in that namespace.
- `allocation_count_quota` limits the number of live allocations in the namespace.
- `zeroing` sets the namespace default for the zeroing policy.
- `allow_export` and `allow_import` gate cross-namespace export/import authorization (see [INTEROP.md](INTEROP.md) and [SECURITY.md](SECURITY.md)). Note the runtime currently authorizes import at the namespace level and rejects *cross-namespace* import outright (see [INTEROP.md](INTEROP.md)).

The default namespace (`kDefaultNamespace` = `1`, named `"default"`) is always present and has no quotas (all `0`), `ON_CROSS_OWNER_REUSE` zeroing, and export/import allowed. Additional namespaces are registered at construction from `RuntimeConfig::namespaces` or at runtime via `Runtime::create_namespace`.

---

## Pooling and capacity

The pool is keyed by `PoolKey` (domain, backend, device, size class, alignment, exportable) and bounded by `PoolConfig::max_idle_bytes` (default 256 MiB) and `PoolConfig::max_objects` (default 4096). Pooled memory is *counted as committed* in its domain until it is trimmed (`pool_trim`) or reused. A buffer released to the pool keeps its domain capacity/reservation held (as idle pooled), so a pool holds domain capacity even though no `BufferHandle` is using it. Use `pool_trim` to return it to the OS.

See [Pooling and zeroing](DESIGN.md#zeroing-policy) and [Pool](ARCHITECTURE.md#pool).
