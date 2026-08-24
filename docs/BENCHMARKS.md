# Benchmarks

This document lists the benchmark categories that Unified Buffer is measured against, how each is measured, and how to run
the benchmark executable. **No results are published here** — numbers are produced by the benchmark executable and are not
reproduced in this documentation. Do not infer performance characteristics from this document.

The top-level [CMakeLists.txt](../CMakeLists.txt) gates the benchmark target with `UNIFIED_BUFFER_BUILD_BENCHMARKS`
(`ON` by default). The target directory is [`benchmarks/`](../benchmarks/).

> **Status note.** The `benchmarks/CMakeLists.txt` is preliminarily scaffolded and the benchmark target is not
> yet implemented in this release, so no results are published here.
> The category list and measurement methodology below are what the benchmark harness targets; there is no published
> result set yet, and this document deliberately contains **no fabricated numbers**.

---

## How to run

Build and run the benchmark executable (regardless of configuration):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build --config Release
build\Release\benchmarks\<benchmark-target>.exe
```

(The exact executable name and the flags it accepts are defined by the benchmark target; see the target's own help output.)

Benchmarks are sensitive to configuration, CPU, and whether CUDA is present. When the CUDA backend is compiled and a device
is available, the CUDA categories are run; when it is not, those categories are skipped.

---

## Measurement methodology

- **Latency** is reported as ns/op or µs/op (per-operation wall-clock, minimum/median/mean over many iterations).
- **Throughput / bandwidth** is reported in GiB/s (or MB/s) computed as bytes moved per unit time.
- **Scaling** is measured over a varying thread count or iteration count to expose contention.
- Measurements that depend on the OS (shared memory, file-backed) are separated from pure in-process ones.

Warm-up iterations are used before measurement so that the pool and allocator caches reach a steady state (relevant for
pooled/latency categories).

---

## Host-only categories

| Category | What is measured | Unit |
| --- | --- | --- |
| Host alloc/free latency | `host` domain `allocate` then `release`, excluding zeroing where configured. | ns/op |
| Pooled alloc/free latency | Allocate with `pooled = true`, release to the pool, allocate again. | ns/op |
| Pool hit rate | Repeated same-class alloc after a warm-up; ratio of pool hits to total allocs. | % |
| Aligned allocation | Allocation at various power-of-two alignments (64, 256, 512, 4096). | ns/op |
| Small buffer throughput | Allocate/map/write/release for small buffers (e.g. 1 KiB, 64 KiB). | ops/s or GiB/s |
| Large buffer throughput | Same for large buffers (e.g. 4 MiB, 64 MiB). | GiB/s |
| Host memcpy bandwidth | `copy_to` and `copy_from` over a host buffer. | GiB/s |
| View creation latency | `view(offset, length, access)` plus release. | ns/op |
| Metadata lookup latency | `Runtime::stats()` / registry lookup over a live set. | ns/op |
| Concurrent allocation throughput | Many threads allocating/releasing concurrently in `HOST`. | ops/s (scaling) |
| Allocator scaling | Throughput vs thread count; exposes lock contention. | ops/s vs threads |

These measure the in-process path and the reuse pool. They do not include OS scheduling noise beyond what the machine
introduces.

---

## Shared memory and file-backed categories

| Category | What is measured | Unit |
| --- | --- | --- |
| Shared-memory mapping latency | Time to allocate `SHARED_HOST` and map it (file-mapping object create plus map). | µs/op |
| Shared-memory copy bandwidth | `copy_from` / `copy_to` over a shared-memory buffer. | GiB/s |
| Shared-memory export/import | `export_buffer` then `import` in the same or a second runtime. | µs/op |
| File-backed mapping latency | Time to allocate `MMAP_STORAGE` and map a file. | µs/op |
| File-backed bandwidth | Copy bandwidth over a file-backed buffer. | GiB/s |
| File-backed reopen | Import a previously exported file path. | µs/op |

These have OS-mediated cost beyond the in-process bookkeeping, so they are reported separately from the host categories.

---

## CUDA categories (when compiled in)

These run only when `UNIFIED_BUFFER_ENABLE_CUDA` found the toolkit and a device is available at runtime:

| Category | What is measured | Unit |
| --- | --- | --- |
| Device alloc/free latency | `DEVICE` domain `allocate` (`cudaMalloc`) then `free` (`cudaFree`). | ns/op |
| Pooled device alloc latency | Device allocation with pooling enabled, if the pool services `DEVICE`. | ns/op |
| H2D bandwidth | `copy_from` to a device buffer (`cudaMemcpy` HostToDevice plus sync). | GiB/s |
| D2H bandwidth | `copy_to` from a device buffer (`cudaMemcpy` DeviceToHost plus sync). | GiB/s |
| D2D bandwidth | In-domain device copy (`cudaMemcpy` DeviceToDevice plus sync). | GiB/s |
| Pinned-vs-pageable staging | `PINNED_HOST` vs `HOST` as staging for H2D. | GiB/s (relative) |
| Migration latency | `Runtime::migrate` host to device (and back). | µs/op |

> Note: there is **no CUDA IPC** in this release, so there is no IPC-based sharing/transfer benchmark. Device memory is
> copied to and from host; it is not peer-to-peer (no `cudaMemcpyPeer`) and not shared across processes.

---

## Interpreting results

- **Pool reuse** mainly reduces the *allocation* latency after a warm-up and reduces address-space churn; it does not
  improve raw copy bandwidth.
- **Zeroing policy** adds cost on the allocation/reuse path for `ON_ALLOCATE` / `ALWAYS`, and on cross-owner reuse, while
  `NEVER` skips zeroing; `ON_RELEASE`/`ALWAYS` zero pooled backing when it is returned to the pool.
- **Mapped address** is a discovery, not a copy; mapping cost depends on the domain (host is nearly free, shared/file
  involve the OS).
- **Concurrent allocation** captures the contention on `pool_mtx_`, `ns_mtx_`, and the registry `shared_mutex`.

Report the machine, configuration, and whether CUDA was used alongside any measured numbers; results are not portable
across hosts.