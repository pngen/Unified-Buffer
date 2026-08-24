# Ownership

This document describes ownership semantics in Unified Buffer 1.0.0: the explicit `Ownership` model, how external
memory is wrapped (borrow / adopt / shared), import ownership, the no-double-free guarantee, and the rule that
**borrowed memory is never freed**.

The ownership enum and the external-memory descriptor are declared in [core/types.hpp](../include/unified_buffer/core/types.hpp)
and [runtime.hpp](../include/unified_buffer/runtime.hpp).

---

## The ownership model

Every buffer has exactly one `Ownership` value, set at creation and never ambiguous. It answers the question
**"who is responsible for releasing the backing memory?"**

| `Ownership` | Set when | Runtime frees the backing? |
| --- | --- | --- |
| `RUNTIME` | A runtime-owned allocation. | **Yes.** |
| `BORROWED` | External memory wrapped with `ExternalOwnership::BORROW`. | **Never.** |
| `ADOPTED` | External memory wrapped with `ExternalOwnership::ADOPT`. | **Yes.** |
| `IMPORTED` | A buffer reconstructed from an export descriptor. | **Yes** (releases the import/handle). |
| `SHARED` | External memory wrapped with `ExternalOwnership::SHARED`. | **Yes** (via the external handle wrapper). |

The runtime decides what to do at finalization based on this single field. That is the crux of the model: ownership is a
per-buffer, explicitly declared property, not an inference from "maybe the pointer was heap-allocated".

---

## Where the backing comes from

There are three ways a buffer gains a backing, and they have different ownership defaults:

### 1. Runtime allocation (`Runtime::allocate`)

`AllocationRequest` produces a buffer with `Ownership::RUNTIME`. The runtime allocated the backing from a domain
backend, so the runtime owns it and frees it at finalization (or returns it to the pool). This is the default and most
common path.

### 2. External memory wrapper (`Runtime::wrap_external`)

`ExternalMemoryDesc` describes memory the caller already owns. The caller must declare the transfer intent via
`ExternalOwnership`:

```cpp
enum class ExternalOwnership : std::uint8_t { BORROW, ADOPT, SHARED };

struct ExternalMemoryDesc {
  void* pointer = nullptr;
  std::uint64_t size = 0;
  std::uint64_t alignment = 0;
  MemoryDomain domain = MemoryDomain::HOST;
  DeviceId device;
  ExternalOwnership ownership = ExternalOwnership::BORROW;   // default: borrow
};
```

The `ExternalOwnership` maps directly to `Ownership`:

| `ExternalOwnership` | `Ownership` | Access mode chosen | Runtime frees? |
| --- | --- | --- | --- |
| `BORROW` | `BORROWED` | `READ` | **No, never.** |
| `ADOPT` | `ADOPTED` | `READ_WRITE` | Yes. |
| `SHARED` | `SHARED` | `READ_WRITE` | Yes (via wrapper). |

Notes on `wrap_external`:

- The pointer must be non-null and the size non-zero, else `invalid_argument`.
- The destination domain must be enabled, else `unsupported_domain`.
- The buffer is registered in the **default namespace** (`kDefaultNamespace`).
- `zero_on_alloc` and `pooled` are both forced to `false`: an external buffer is never zeroed or pooled by the runtime.
- Alignment defaults to `64` if not provided.
- The pointer becomes both the `NativeAllocation::pointer` and `host_map` for CPU-accessible domains.

### 3. Import (`Runtime::import`)

`import` reconstructs a buffer from an `ExportDescriptor`. The resulting buffer has `Ownership::IMPORTED`. The runtime
owns the *import*: for shared memory and file-backed domains it owns the opened mapping/file handle and will release it
at finalization; for the same-runtime local case it re-shares the existing control block, so it is not a new backing at
all.

---

## Borrowed memory is never freed

This is the single most important ownership rule. When a buffer has `Ownership::BORROWED`, the runtime treats its
backing as caller-owned. At finalization:

```cpp
bool backing_present = rec.backing.pointer != nullptr || rec.backing.host_map != nullptr;
if (backing_present && rec.ownership != Ownership::BORROWED) {
  // ... pool or free ...
} else {
  rec.backing = NativeAllocation{};   // BORROWED: just clear the descriptor, never free
}
```

So for a borrowed buffer, finalization **clears the `NativeAllocation`** (the runtime "forgets" it) but never calls
the backend's `free`. The caller retains ownership of the wrapped memory and remains responsible for releasing it. The
runtime never frees, reuses, or re-pools borrowed memory.

---

## Adopted and shared memory

- **`ADOPTED`** means the caller transfers ownership of the pointer to the runtime. At finalization the runtime treats it
  like a runtime allocation: it sets the ownership to `RUNTIME` and calls the domain `free` on the backing. After that
  the caller must not use the pointer.
- **`SHARED`** means the buffer wraps a shared handle (for example, an external handle wrapper). At finalization the
  runtime releases the backing through the domain `free`, which in the shared/file backends releases the mapping/file
  handle. The wrapper is expected to keep the same lifetime intent.

Both adopt and shared end with the runtime freeing the backing; the distinction is the caller intent. `ADOPT` is
"I hand you this and you own it"; `SHARED` is "we share it and you hold a handle you must eventually free".

---

## Import ownership

An imported buffer is created with `Ownership::IMPORTED`. There are two import paths and therefore two meanings of
"release":

1. **Shared-memory / file-backed import** (`SHARED_HOST`, `MMAP_STORAGE`). The runtime called the domain `import_handle`
   and received a freshly opened mapping/file. The runtime owns that import and will release it (unmap/close) at
   finalization by calling the domain `free`. This is the physical release of the process-local handle.
2. **Same-runtime local import** (`handle_kind == "local"`, for `HOST` / `PINNED_HOST` / `DEVICE`). The runtime looks up
   the existing control block by `(buffer_id, generation)` and re-shares it. There is no new backing; the imported
   handle simply retains into the same control block. Releasing the imported handle drops one reference, exactly like
   `share()`. This is only valid within the same runtime instance and only when the descriptor resolves to that buffer
   (a host/device descriptor from a *different* runtime is refused).

An imported buffer is never pooled (`pooled` is forced to `false`).

---

## No-double-free

The runtime guarantees a backing is released at most once, in complementary ways:

1. **The `finalized` atomic flag.** `finalize` runs `finalized.exchange(true)` before doing any work; if it was already
   true, it returns immediately. So even if `finalize` is reached twice through different paths, the backing is freed at
   most once.
2. **Registry removal.** While finalizing, the control block grabs a unique registry lock and erases its entry by
   `BufferId`. Once erased, a `lookup` of that id returns `stale_handle`, so the buffer cannot be located and finalized
   again by id.
3. **Backing is cleared after release.** After `free`, the code assigns `rec.backing = NativeAllocation{}`, so the
   pointer is no longer held. A second `finalize` sees `backing_present == false` and takes the clear-only branch.

Together, these make double-free of a backing a non-event rather than a crash.

---

## Ownership validation on import

`import` performs several validations before accepting a descriptor (see also [INTEROP.md](INTEROP.md) and
[SECURITY.md](SECURITY.md)):

- The descriptor format version must equal `kExportFormatVersion` (`1`), else `import_failure`.
- The size must be non-zero and at most `2^48`, else `import_failure`.
- If an alignment is given, it must be a power of two, else `import_failure`.
- The **target namespace** must exist; the **source namespace** must allow import; the **target** must allow import; and a
  **cross-namespace import** (`desc.ns != ns`) is refused with `permission_failure`.
- For shared/file domains, the imported segment `committed` must be at least the declared `size`, else `import_failure`.
- The metadata CRC is compared, but a mismatch is **recorded, not rejected** (the handle id may legitimately differ across
  runtimes); content integrity is verified explicitly via `verify()`.

Import does **not** trust the descriptor provenance (`label` is human-readable only) and does **not** accept raw process
pointers as portable handles. The descriptor never carries a raw pointer for cross-process exchange; only the opaque
domain handle (shared name or file path) crosses the boundary.

---

## Summary of the never-free and no-double-free rules

| Buffer kind | Ownership | Freed by runtime? | Notes |
| --- | --- | --- | --- |
| `allocate` | `RUNTIME` | Yes | Pooled if eligible, else freed. |
| `wrap_external` (borrow) | `BORROWED` | **No** | Descriptor cleared; caller frees. |
| `wrap_external` (adopt) | `ADOPTED` | Yes | Converted to `RUNTIME` then freed. |
| `wrap_external` (shared) | `SHARED` | Yes | Freed via domain `free`. |
| `import` (shared/file) | `IMPORTED` | Yes | Releases the opened handle. |
| `import` (local) | `IMPORTED` | Yes | Drops one reference on the existing control block. |
