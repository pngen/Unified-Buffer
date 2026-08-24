# Interop

This document describes how buffers cross a boundary in Unified Buffer 1.0.0: the versioned export/import descriptor,
what actually crosses the boundary, what does not, the same-process host/device restrictions, interprocess sharing via
`SHARED_HOST`, file-backed reopen via `MMAP_STORAGE`, borrowed CUDA device-pointer access, and DLPack/framework adapter
guidance.

The descriptor and version constant are in [export.hpp](../include/unified_buffer/export.hpp). The export/import logic is
in [src/export.cpp](../src/export.cpp).

---

## The export/import descriptor (format version 1)

`kExportFormatVersion == 1`. The descriptor is a plain, serializable struct:

```cpp
struct ExportDescriptor {
  std::uint32_t format_version = kExportFormatVersion;
  BufferId buffer_id;
  BufferGeneration generation = 0;
  NamespaceId ns = kDefaultNamespace;
  BackendId backend = BackendId::HOST_MALLOC;
  MemoryDomain domain = MemoryDomain::HOST;
  DeviceId device;
  std::uint64_t size = 0;
  std::uint64_t alignment = 0;
  AccessMode access = AccessMode::READ_WRITE;
  std::string handle_kind;   // "shared", "file", "host", "device", "ipc"
  std::string handle;        // opaque, backend-specific handle data
  std::uint32_t integrity_crc = 0;
  bool exportable = false;
  bool writable = true;
  std::uint32_t policy_version = 1;
  std::string label;         // human-readable provenance, not trusted at import
};
```

| Field | Meaning | Trusted at import? |
| --- | --- | --- |
| `format_version` | Must equal `1`. | Yes (hard requirement). |
| `buffer_id` | The stable id in the exporting runtime. | Used for same-runtime re-share; a fresh id is minted on shared/file import. |
| `generation` | The generation recorded at export. | Used for same-runtime re-share. |
| `ns` | The source namespace. | Used for authorization. |
| `backend`, `domain`, `device` | Where the buffer lives. | Used to pick the backend and validate the domain. |
| `size`, `alignment` | The logical byte extent and alignment. | Validated (non-zero, bounded, power-of-two alignment). |
| `access` | The allowed access mode. | Carried to the imported buffer. |
| `handle_kind`, `handle` | The opaque domain handle. | Backend-specific; validated by the backend. |
| `integrity_crc` | A metadata checksum (not a content hash). | Compared; mismatch is recorded, not rejected. |
| `exportable`, `writable` | Flag and write permission. | `writable` derived from `access`. |
| `policy_version` | Policy version (always `1`). | Reserved. |
| `label` | Human-readable provenance. | **Not trusted.** |

---

## What crosses the boundary

Only the *identifier, metadata, and an opaque domain handle* cross the boundary. Nothing here is a raw process pointer
that can be blindly dereferenced in another context.

- **For `SHARED_HOST`:** `handle_kind = "shared"`, `handle` = the Windows **file-mapping name** (e.g.
  `UnifiedBuffer_<seed>_<counter>`). `import_handle` opens it with `OpenFileMappingA` and maps it.
- **For `MMAP_STORAGE`:** `handle_kind = "file"`, `handle` = the **file path** in the base directory. `import_handle`
  opens it, validates traversal, and maps it.
- **For `HOST` / `PINNED_HOST` / `DEVICE`:** `handle_kind = "local"`, `handle` = the string form of the exporting
  `RuntimeImpl` address. This is **not** a portable handle; it only lets the *same runtime instance* re-share the
  existing control block by `(buffer_id, generation)`.

---

## What does NOT cross the boundary

- **Raw process pointers.** The descriptor never carries a `void*` payload pointer as a portable handle. A pointer is
  only meaningful within the process that mapped it, so it is never serialized for cross-process exchange.
- **The `NativeAllocation` / backend state.** The OS handle (mapping, file) is reconstructed on import; the in-process
  `NativeAllocation` and its `state` are not transmitted.
- **Trusted provenance.** `label` and the `handle` string are treated as untrusted input at import.
- **CUDA IPC.** There is no `cudaIpcMemHandle` / IPC channel in 1.0.0. Device buffers are exported as `"local"` only and
  cannot be shared across processes.

---

## Same-process host/device import restrictions

For `HOST`, `PINNED_HOST`, and `DEVICE`, the descriptor is a **same-process re-share** token, not a transferable handle.

-- Import of a `"local"` descriptor resolves only inside the same runtime instance: `import_descriptor` looks up
   `(buffer_id, generation)` in the registry. If the descriptor came from a *different* runtime, the lookup fails and
   import returns `import_failure`.
-- The host/device descriptor does not carry enough information to reconstruct memory in another process. There is no
   cross-process host or device sharing in 1.0.0. If you must share host memory across processes, use `SHARED_HOST`.
-- The `device` field is carried, but a device memory buffer cannot be CPU-mapped and its host-side re-share is the
   same-runtime control block.

### Why the address is not a portable handle

Exporting the raw pointer would let a foreign process dereference a pointer that is meaningless there. The runtime
deliberately refuses to serialize raw pointers as handles and instead requires a domain that can produce a real OS-managed
handle (shared mapping name, file path) for cross-process exchange.

---

## Interprocess sharing via `SHARED_HOST`

This is the supported path for sharing memory between processes:

1. **Producer.** Allocate a buffer in `SHARED_HOST` with `flags.exportable = true`. A `SHARED_HOST` allocation creates a
   named file-mapping object and maps it. `export_buffer` returns a descriptor whose `handle_kind` is `"shared"` and
   whose `handle` is the mapping name.
2. **Transport.** Hand the descriptor to the consumer process (out of band). The consumer constructs an `ExportDescriptor`
   with the same `handle` (or deserializes it from your wire format).
3. **Consumer.** `Runtime::import(desc)` opens the mapping by name (`OpenFileMappingA`) and maps the same bytes. The
   imported buffer has a *fresh* `BufferId` and `Ownership::IMPORTED`; it points at the same physical mapping.

Because `SHARED_HOST` maps the same backing, reads and writes by either process are visible to the other (the domain
advertises `COHERENT`). Neither process owns the other's reference: each holds its own handle to the shared mapping and
releases it independently.

---

## File-backed reopen via `MMAP_STORAGE`

When you allocate in `MMAP_STORAGE`, the runtime creates a file in the base directory and maps it. `export_buffer` returns
`handle_kind = "file"`, `handle = <path>`. A consumer can import that path later:

1. `import` opens the file, checks the declared size against the file size, and maps it.
2. Path-traversal protection rejects a path containing `..` or a traversal segment, rejects paths over 1024 characters,
   and requires the path be beneath the configured base directory.
3. The file can be reopened by the same or another process (subject to the file still existing and having adequate size).
   Note the runtime does **not** itself persist the descriptor catalog; the path is the handle.

Flush is explicit: `BufferMap::release` does not flush, but the file domain's `flush()` (via `FlushViewOfFile` and
`FlushFileBuffers` on Windows) pushes mapped bytes to disk when you call it.

---

## CUDA device-pointer borrowed access

A `DEVICE` buffer is not CPU-addressable, so it is not mapped. To use the bytes from a kernel, call
`BufferHandle::device_pointer()`:

```cpp
auto dp = h.device_pointer();   // Result<void*>
if (dp.ok()) {
  void* ptr = dp.value();       // borrowed, tied to this handle + generation
  // pass ptr to a kernel / cudaMemcpy
}
```

The returned pointer is **borrowed**: it is valid only while the handle is alive and the generation matches. It must not
be stored past the buffer lifetime or across a migration/release. Device memory cannot be exported for cross-process use
in this release — the descriptor is `"local"`.

---

## DLPack / framework adapter guidance

Unified Buffer does not implement a framework adapter protocol in 1.0.0. If you are building an adapter between DLPack
(or a framework tensor) and Unified Buffer, treat the following as the contract:

### Mapping a DLPack tensor to a Unified Buffer

- Wrap the DLPack-managed memory with `Runtime::wrap_external`. The `ExternalMemoryDesc` must declare whether the runtime
  should borrow (`BORROW`, never freed), adopt (`ADOPT`, runtime frees), or share (`SHARED`). DLPack's `DLManagedTensor`
  carries a deleter; choose the ownership that matches who is responsible for releasing it.
- Set `domain` to the domain the memory actually lives in (HOST, PINNED_HOST, DEVICE, and so on) so the runtime picks the
  correct backend and capability set.
- For device tensors, use `device_pointer()` to obtain the pointer for the consumer; do not expect `map()` to succeed.

### Exporting a Unified Buffer to DLPack / a framework

- Use the `ExportDescriptor` as the transfer medium. It carries identity, generation, domain, size, alignment, access, and
  an opaque handle — not a raw pointer.
- For a cross-process descriptor, prefer `SHARED_HOST` (or `MMAP_STORAGE` for persistence); do **not** attempt to
  cross-process a `"local"` host/device descriptor.
- Validate ownership on the import side (see [Ownership validation on import](#ownership-validation-on-import)) before
  releasing any memory.

---

## Ownership validation on import

Before an imported buffer is accepted, `import` validates (see also [OWNERSHIP.md](OWNERSHIP.md) and
[SECURITY.md](SECURITY.md)):

- `format_version == kExportFormatVersion`, else `import_failure`.
- `size != 0` and `size <= 2^48`, else `import_failure`.
- `alignment` is a power of two if set, else `import_failure`.
- The **target namespace** must exist, the **source namespace** must allow import, the target must allow import, and a
  **cross-namespace** import (`desc.ns != ns`) is refused with `permission_failure`.
- For shared/file domains, the imported segment `committed >= size`, else `import_failure`.
- The metadata `integrity_crc` is compared, but a mismatch is recorded (not rejected) because a freshly minted id in a new
  runtime legitimately differs.

Import never trusts `label`, and never trusts the descriptor to contain a usable raw pointer. Content integrity is checked
explicitly via `BufferHandle::verify()`.

---

## Summary table

| Target domain | `handle_kind` | What crosses | Cross-process? | Cross-runtime (same process)? |
| --- | --- | --- | --- | --- |
| `HOST` | `"local"` | Runtime-impl token | No | No (same runtime only) |
| `PINNED_HOST` | `"local"` | Runtime-impl token | No | No (same runtime only) |
| `DEVICE` | `"local"` | Runtime-impl token | No | No (same runtime only) |
| `SHARED_HOST` | `"shared"` | Mapping name | Yes | Yes |
| `MMAP_STORAGE` | `"file"` | File path | Yes | Yes |
