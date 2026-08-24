# Architecture

This document describes the structure of Unified Buffer as it exists in 1.0.0: repository layout, the layers, the backend abstraction, the registry and control block, lifecycle, locking, per-domain accounting, the reuse pool, and the boundary against adjacent systems.

---

## Repository layout

```
Unified-Buffer/
├── CMakeLists.txt                      # top-level build; defines the library, CLI, tests, examples, benchmarks
├── LICENSE                             # Apache-2.0
├── README.md
├── include/
│   └── unified_buffer/
│       ├── runtime.hpp                 # Runtime facade, AllocationRequest, ExternalMemoryDesc
│       ├── buffer.hpp                  # BufferHandle, BufferLease, BufferMap, BufferView
│       ├── export.hpp                  # ExportDescriptor, kExportFormatVersion
│       ├── telemetry.hpp               # BackendInfo, DeviceInfo, DomainInfo, RuntimeStats
│       ├── config.hpp                  # RuntimeConfig, PoolConfig, NamespaceConfig
│       ├── version.hpp                 # version_string()
│       ├── core/
│       │   ├── types.hpp               # MemoryDomain, BackendId, DeviceId, AccessMode, BufferState, Ownership
│       │   ├── identity.hpp            # BufferId, BufferGeneration, NamespaceId, kDefaultNamespace
│       │   ├── error.hpp               # ErrorCode, Error
│       │   ├── result.hpp              # Result<T>, Status
│       │   ├── capabilities.hpp        # Capability, Capabilities
│       │   ├── checked_math.hpp        # add_overflow, mul_overflow, align_up, range_in_bounds
│       │   └── integrity.hpp           # crc32c, crc32c_self_test
│       └── backends/
│           ├── domain.hpp              # IDomain, NativeAllocation, CapacityInfo (backend interface)
│           ├── host_backend.hpp        # HostDomain
│           ├── cuda_backend.hpp        # CudaDeviceDomain, CudaPinnedDomain, cuda_support
│           ├── shared_memory_backend.hpp # SharedMemoryDomain
│           └── file_backend.hpp        # FileBackedDomain
├── src/
│   ├── internal.hpp                    # RuntimeImpl, Record, Control, PoolSlot, PoolKey (private)
│   ├── runtime.cpp                     # RuntimeImpl: lifecycle, pooling, accounting, migration
│   ├── buffer.cpp                      # BufferHandle/Lease/Map/View behaviour
│   ├── export.cpp                      # ExportDescriptor/import dispatch, meta_crc_of
│   ├── stats.cpp                       # runtime_stats, backends/devices/domains info
│   ├── platform.cpp                    # attach_shared_and_file_backends
│   ├── version.cpp
│   ├── core/
│   │   ├── copy_memory.cpp             # host/device copy dispatch
│   │   └── integrity.cpp               # CRC-32C
│   └── backends/
│       ├── host_backend.cpp
│       ├── cuda_backend.cpp
│       ├── shared_memory_backend.cpp
│       └── file_backend.cpp
├── cli/main.cpp                        # prints the runtime version
├── tests/                              # CTest targets (test_*.cpp)
├── examples/                           # example targets (scaffolded; target not yet implemented)
└── benchmarks/                         # benchmark targets (scaffolded; target not yet implemented)
```

The build is a single static library (`unified_buffer`), a thin CLI, and registered test/example/benchmark targets. See the top-level [CMakeLists.txt](../CMakeLists.txt).

---

## Layers

Unified Buffer is structured as a small set of layers that call downward.

1. **Public API layer** — `Runtime`, `BufferHandle`, `BufferLease`, `BufferMap`, `BufferView`, and the config/descriptor/telemetry structs in `include/unified_buffer/`. These are the only headers a consumer includes. They are move-only RAII handles that hold a `std::shared_ptr` to the internal implementation so handles can outlive the `Runtime` facade object (while the runtime is marked closed).
2. **Core layer** — `Result<T>`, `ErrorCode`, checked arithmetic, the 128-bit `BufferId`, and capabilities. This layer is value/type only; it has no global state.
3. **Runtime layer (`RuntimeImpl`)** — the shared implementation in `src/internal.hpp` and `src/runtime.cpp`. It owns the registry of live buffers, the pool, namespaces/quota accounting, the lifecycle transition table, and the backend set.
4. **Backend layer (`IDomain`)** — one object per (domain, backend) pair implemented in `src/backends/*.cpp`. The runtime talks only to `IDomain`. Backends are deliberately opaque: they allocate/free/zero/map/copy and report capacity and capabilities.

---

## The `IDomain` backend abstraction

`IDomain` (in `include/unified_buffer/backends/domain.hpp`) is the single seam between the runtime and any concrete memory platform. A backend implements one or more domains. The interface is small and side-effect-scoped:

```cpp
virtual BackendId     backend() const = 0;
virtual MemoryDomain  domain() const = 0;
virtual const Capabilities& capabilities() const = 0;

virtual Result<NativeAllocation> allocate(std::uint64_t bytes, std::uint64_t alignment) = 0;
virtual Result<std::uint64_t>    free(NativeAllocation&) = 0;

virtual Status zero(NativeAllocation&, std::uint64_t offset, std::uint64_t len) = 0;
virtual Status synchronize(NativeAllocation&) = 0;
virtual Status flush(NativeAllocation&) = 0;
virtual Status verify(NativeAllocation&, std::uint64_t offset, std::uint64_t len) = 0;

virtual Result<void*> map(NativeAllocation&, AccessMode) = 0;
virtual Status         unmap(NativeAllocation&, void*) = 0;

virtual Status copy_in_domain(NativeAllocation& dst, std::uint64_t dst_off,
                              const NativeAllocation& src, std::uint64_t src_off,
                              std::uint64_t len) = 0;

virtual Result<CapacityInfo> capacity() const = 0;
virtual Result<std::uint64_t> preferred_alignment() const = 0;
virtual Result<std::uint64_t> default_alignment() const = 0;

virtual Result<std::string>      export_handle(const NativeAllocation&) const;
virtual Result<NativeAllocation> import_handle(const std::string&, std::uint64_t, std::uint64_t) const;
```

Key rules of the abstraction:

- **Capabilities are explicit.** A backend advertises what it supports in its `Capabilities`. The runtime consults `capabilities()` and rejects an unsupported operation with `ErrorCode::unsupported_capability` *before* doing side effects. It never silently emulates.
- **Cross-domain copies are composed by the runtime.** `copy_in_domain` is a same-domain copy. Cross-domain copies (e.g. host to device) are built by the runtime in `copy_memory`, which dispatches to `cudaMemcpy` when either side is a device and falls back to `std::memcpy` otherwise.
- **`export_handle` / `import_handle` have a default that fails.** Only domains that can serialize an external handle (SHARED_HOST, MMAP_STORAGE) override them; the default returns `unsupported_capability`.
- **One domain, one backing.** A `NativeAllocation` is the raw result of a backend: `pointer` (native pointer, device pointer for DEVICE), `host_map` (CPU alias, null for device memory), `size` (requested payload bytes), `committed` (bytes actually committed to the backing), `alignment`, `device`, `domain`, `backend`, and an opaque `std::shared_ptr<void> state` that keeps platform objects alive (e.g. a mapping handle).

Backends implemented in 1.0.0:

| Backend (`BackendId`) | Domain(s) | Notes |
| --- | --- | --- |
| `HOST_MALLOC` | `HOST` | `_aligned_malloc` and `_aligned_free`, pageable. |
| `CUDA` | `DEVICE`, `PINNED_HOST` | `cudaMalloc` and `cudaFree`, `cudaHostAlloc` and `cudaFreeHost`. Only compiled when the CUDA Toolkit is found. |
| `WIN_SHARED_FILE` | `SHARED_HOST` | Windows file-mapping objects (named). |
| `FILE_BACKED` | `MMAP_STORAGE` | Memory-mapped files in a base directory. |

---

## Registry and control block

The runtime keeps a **registry** — an `unordered_map<BufferId, shared_ptr<Control>>` guarded by a `shared_mutex` (`registry_mtx_`). The registry is the authoritative answer to "is this buffer alive, and what is its current metadata?"

Each live buffer has a **`Control`** block (in `src/internal.hpp`):

```cpp
struct Control : std::enable_shared_from_this<Control> {
  std::shared_ptr<RuntimeImpl> rt;
  std::mutex mtx;              // serializes mutation of this single buffer
  Record rec;                  // the public metadata for the buffer
  std::atomic<int> refs{0};    // every external reference (handles + leases/maps/views)
  std::atomic<bool> finalized{false};
  int read_leases = 0;
  int write_leases = 0;
  int exclusive_leases = 0;
  bool pool_eligible = false;
};
```

The `Record` is the per-buffer metadata: id, generation, namespace, domain, backend, device, size, committed, alignment, `AllocationFlags`, access, pool eligibility, `Ownership`, `BufferState`, the `NativeAllocation` backing, pooled/exported flags, map count, created epoch, label, and a metadata CRC.

Two invariants tie the registry and control block together:

- **Identity is stable.** `Record::id` never changes for the life of a buffer; the address (`backing.pointer`) can change (migration).
- **Generation is authoritative.** A handle is only usable while its stored generation equals `Record::generation`. On migration, generation advances.

---

## Buffering lifecycle

The lifecycle is a `BufferState` state machine implemented with an explicit legal-transition table (`RuntimeImpl::valid_transition`). The detail is in [LIFECYCLE.md](LIFECYCLE.md); the architectural points are:

- The transition table is **the** authority. `set_state` and `transition` reject an illegal move with `ErrorCode::lifecycle_violation`.
- Some internal paths assign state directly (allocation sets `ALLOCATED`, import sets `IMPORTED`, migration resets to `ALLOCATED`) but never perform an illegal move; the table governs what is legal and is enforced wherever a transition is staged.
- **`refs` is the lifetime driver.** `Control::refs` counts every `BufferHandle` plus every active lease/map/view. The buffer is finalized exactly once when `refs` reaches zero (`RuntimeImpl::drop_ref` and `RuntimeImpl::finalize`).
- **Finalization is deterministic.** `finalize` runs under the control mutex, exchanges the `finalized` flag (so it runs at most once), then either returns the backing to the pool or frees it, sets `RELEASED`, releases quota/accounting, and erases the registry entry.

---

## Locking strategy

The runtime uses several distinct scopes of synchronization, chosen to bound contention:

1. **Per-control mutex (`Control::mtx`)** — serializes all metadata mutation for a *single* buffer: state transitions, lease counts, map count, migration, finalization. There is no global "big lock" for buffer metadata.
2. **Registry shared mutex (`registry_mtx_`)** — a `std::shared_mutex`. Lookups take a shared lock; registration and removal take a unique lock. This lets many concurrent lookups proceed in parallel.
3. **Pool mutex (`pool_mtx_`)** — a plain `std::mutex` guarding the pool and the idle-byte/object accounting. Pool operations are short and centralized.
4. **Namespace mutex (`ns_mtx_`)** — a `std::mutex` guarding namespace quota/committed/allocated/reserved accounting.
5. **Telemetry and decision mutexes** — `telemetry_mtx_` and `dec_mtx_` guard the counters and the bounded decision log.

Lock ordering is deliberately shallow: per-control mutations do not hold registry or pool locks for their full lifetime, and pool/namespace accounting is done with dedicated locks rather than nesting per-control locks inside them. Cross-buffer coordination is via leases at the per-control level.

---

## Per-domain accounting and quotas

The runtime accounts committed/reserved/allocated bytes per namespace and per domain, and enforces two tiers of limits:

- **Per-domain capacity caps** (`RuntimeConfig::host_cap`, `pinned_cap`, `device_cap`, `shared_cap`, `file_cap`). These are the hard ceiling for a domain; a value of `0` means "use the backend-reported/default capacity". `capacity_for` returns `min(configured_cap, backend_total)` when both are nonzero.
- **Per-namespace quotas** (`NamespaceConfig::host_quota`, `pinned_quota`, `device_quota`, `shared_quota`, `file_quota`, `allocation_count_quota`). A namespace quota of `0` means unlimited for that domain.

The accounting states tracked per (namespace, domain) are `reserved`, `committed`, and `allocated`:

- `reserve_amount` reserves bytes and fails with `out_of_capacity` (domain cap) or `quota_exceeded` (namespace quota).
- `commit_amount` converts a reservation into committed plus allocated and updates the peak.
- `checkout_amount` moves idle pooled bytes back into the allocated bucket.
- `release_amount` returns bytes to the allocated/committed pool (for a pooled free) or reduces committed (for a direct free).
- `free_accounting` reduces both allocated and committed for a direct free.

Byte counters drive the telemetry in `RuntimeStats` (host_bytes, pinned_host_bytes, device_bytes, shared_host_bytes, file_backed_bytes, peak, committed, and so on).

---

## Pool

The reuse pool is a map from `PoolKey` to `deque<PoolSlot>`, guarded by `pool_mtx_`.

```cpp
struct PoolKey {
  MemoryDomain domain;
  BackendId backend;
  DeviceId device;
  std::uint64_t size_class;
  std::uint64_t alignment;
  bool exportable;
};

struct PoolSlot {
  NativeAllocation backing;
  BufferId original_id;
  NamespaceId last_owner;
  bool zeroed;
};
```

- `size_class_for` rounds a requested byte count up to the next power-of-two within `[min_class_size, max_class_size]` (defaults 4096 and 256 MiB).
- `pool_take` pops a matching slot and records a hit; a miss increments `pool_misses`.
- `pool_put` pushes a slot only if the pool is under `max_objects` and `max_idle_bytes`; otherwise it evicts (frees the backing directly) and counts an eviction.
- A buffer is pool-eligible when pooling is enabled, its size is at least `min_class_size`, and its derived size class is at most `max_class_size`. Both fresh and pooled allocations set `pool_eligible`; a pooled allocation stays `pooled == true`.
- Zeroing on reuse is governed by the namespace `ZeroingPolicy` (or a per-request override). `ZeroingPolicy::ON_CROSS_OWNER_REUSE` zeroes only when a slot is reused by a *different* namespace than its recorded `last_owner`.

---

## What is *not* in the architecture

There is no daemon, no IPC/control protocol, no message bus, and no on-disk catalog. The runtime is an in-process library; interprocess exchange happens only through the OS primitives in the `SHARED_HOST` and `MMAP_STORAGE` backends.

---

## Boundary vs adjacent systems

Unified Buffer deliberately stops at the memory-object boundary. The following systems/concerns are **outside** its scope; they might *consume* Unified Buffer for their memory objects, but they are not implemented here.

### FlashTier / storage tiering
Unified Buffer does not tier hot vs cold data, does not move pages between DRAM and SSD, and makes no residency-eviction decisions. Use a tiering system on top; it can use Unified Buffer as the memory object it promotes/demotes.

### Context Fabric
There is no runtime-managed, host-wide context abstraction, no CUDA context/stream ownership, no per-thread context, and no asynchronous queue. Allocation and copy are synchronous; they return only when the operation has completed.

### Compute Fabric
Unified Buffer does not launch kernels, schedule nodes, or manage a compute graph. The `DEVICE` domain allocates and copies device memory; kernels are launched by the owning framework.

### Transfer Fabric
There is no transfer scheduler, no copy engine arbitration, and no async transfer API. `copy_in_domain` and the runtime’s `copy_memory` are synchronous.

### Topology Fabric
Unified Buffer does not model NUMA domains, PCIe/NVLink topology, or peer-to-peer distance. It does not use `cudaMemcpyPeer`. (There is a `numa_hint` field on `AllocationFlags`, but it is a request hint, not a topology engine; it is carried in the request and not acted on by the current backends.)

### Memory Pressure
There is no pressure signal, no watermark, no prioritization of eviction under memory pressure, and no automatic reclaim of the pool other than explicit `pool_trim`. Quota/capacity enforcement is a hard rejection, not a pressure-based admission policy.

### Memory Observatory
Unified Buffer exposes counters and a bounded decision log (`Runtime::stats()`, `Runtime::decisions()`), but it is not a tracing/observability system. The decision log is a capped in-memory ring (4096 entries) of human-readable events for debugging; it is not a metrics pipeline or an export to a collector.

### Framework adapters (DLPack, and so on)
Unified Buffer does not implement a framework adapter protocol in 1.0.0. See [INTEROP.md](INTEROP.md) for how the export/import descriptor can be adapted to such protocols and for the ownership validation an adapter must perform.

In short: Unified Buffer is the layer that answers *"what is this memory object, who owns it, where does it live, and is it still valid?"* — and it is deliberately not the layer that answers *"which data should be where, when, and how should it move."*
