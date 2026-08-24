# Design

This document records the design decisions and the object model behind Unified Buffer 1.0.0, and the invariants the runtime maintains. It explains *why* the API looks the way it does. For the public API reference, see [README.md](../README.md); for structure, see [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Guiding decisions

### 1. Identity is separate from address
The single most important decision is to give a buffer a **stable identity** (`BufferId`, 128 bits) that is independent of its address. The address of a buffer may change (migration, pooling, remapping); the identity must not. This makes a `BufferId` safe to pass across a library, process, or device boundary, because it names the *object*, not the bytes.

### 2. Generation is the staleness guard
A 64-bit **generation** starts at `1` and advances whenever an operation changes the buffer's authoritative binding (currently **migration**). Any handle, lease, map, or view that carries an old generation is rejected with `ErrorCode::stale_generation`. A handle that references a finalized or unknown id is rejected with `ErrorCode::stale_handle`. This eliminates the classic use-after-free pattern where a raw pointer survives a reallocation: the *pointer* is never the authority — the `(BufferId, generation)` pair is.

### 3. A buffer has exactly one backing authority at any time
In 1.0.0, residency is **single-authority**: a buffer lives in exactly one memory domain and owns exactly one backing at a time. `migrate` allocates the destination backing, copies, verifies the destination, swaps the binding, and only then releases the old authority. There is never a moment where two backings claim to be the same buffer. (The design deliberately does not implement split residency, coherent dual residency, or tensor residency in this release.)

### 4. Explicit ownership, no ambiguous ownership
Every buffer has an explicit `Ownership` mode. The runtime frees a buffer only when its ownership allows it. Borrowed memory is **never** freed. This removes the ambiguity of "who frees this?" that plagues raw allocator wrappers.

### 5. Errors are structured, not boolean
Every fallible operation returns a `Result<T>` carrying either a value or an `Error` with an `ErrorCode` and a human-readable message. No failure is collapsed to a boolean `false`; callers can branch on the exact error category. See [Error model](#errormodel).

### 6. Checked arithmetic on all metadata paths
Every size, offset, and alignment computation on a metadata path goes through [checked_math.hpp](../include/unified_buffer/core/checked_math.hpp). Overflow is detected and surfaces as `ErrorCode::overflow` or `ErrorCode::bounds_error`, never as a wrap-around that corrupts accounting. See [Checked arithmetic](#checked-arithmetic).

### 7. Capabilities are advertised and enforced
A domain advertises a `Capabilities` set. The runtime rejects an operation the domain does not support *before* performing side effects, rather than emulating it silently. This keeps behavior honest and makes unsupported operations fail loudly and deterministically.

### 8. Rejection over emulation for cross-domain operations
Cross-domain copies are composed in `copy_memory`, but only for the copy kinds the backend supports (host-to-host via `memcpy`, device-involved via `cudaMemcpy`). A `DEVICE` buffer cannot be CPU-mapped, and `checksum` of device content is refused (it would require a host round-trip). The runtime reports `unsupported_capability` instead of doing an expensive, surprising implicit copy.

---

## Object model

### `BufferId` — 128-bit stable identity

```cpp
struct BufferId {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  bool operator==(const BufferId&) const noexcept;
  bool operator!=(const BufferId&) const noexcept;
  bool operator<(const BufferId&) const noexcept;
  [[nodiscard]] bool null() const noexcept;
};
```

- `BufferId` is the primary, handle-independent identity of a buffer.
- Locally, the runtime mints an id as `hi = seed ^ (epoch & 0xFFFFFFFF)`, `lo = atomic counter`. The seed comes from a `std::random_device` at construction, so ids are not predictable across runs.
- `null()` returns true only for `(0, 0)`, which is never assigned to a live buffer.
- A `std::hash` specialization (splitmix64-style) allows `BufferId` to be used directly as an `unordered_map` key.

### `BufferGeneration` — 64-bit monotonically increasing

A buffer is created with generation `1`. It is bumped (to `generation + 1`) on **migration**. Every operation that carries an expected generation and a handle compares them. The generation is what makes a stale handle harmless: after a migration, an old handle's generation no longer matches.

### `Ownership` — who releases the memory

| Value | Meaning | Runtime frees it? |
| --- | --- | --- |
| `RUNTIME` | A runtime-owned allocation. | Yes. |
| `BORROWED` | External memory whose pointer belongs to the caller. | **Never.** |
| `ADOPTED` | External memory the runtime now owns and must deallocate. | Yes. |
| `IMPORTED` | A buffer reconstructed from an export descriptor. | Yes (releases the import). |
| `SHARED` | Shared ownership via an external wrapper/handle. | Yes. |

See [OWNERSHIP.md](OWNERSHIP.md).

### `AllocationFlags`

```cpp
struct AllocationFlags {
  bool zero_on_alloc = true;   // zero memory after allocation
  bool pooled = true;          // allow pool reuse
  bool exportable = false;     // may be exported / shared
  uint8_t numa_hint = 0;       // 0=default,1=local,2=preferred,3=interleave
};
```

- `zero_on_alloc` — the default is to zero after allocation. It is an input hint; the effective zeroing is decided by the `ZeroingPolicy` (see [Zeroing policy](#zeroing-policy)).
- `pooled` — whether the allocation may be served from or returned to the pool. Set false to force a fresh allocation each time.
- `exportable` — whether the buffer may be exported. `export_buffer` on a non-exportable buffer returns `permission_failure`.
- `numa_hint` — a request hint only. The current backends do not act on it; it is carried for future topology-aware backends.

### `AccessMode`

`READ`, `WRITE`, `READ_WRITE`. A buffer that is created with `READ` access cannot be written; a `WRITE` mapping/lease on it returns `permission_failure`.

---

## Error model

### `ErrorCode` and `Error`

`ErrorCode` is an `int16_t` enum of roughly thirty categories (see [error.hpp](../include/unified_buffer/core/error.hpp)), from `ok` and `invalid_argument` through `out_of_capacity`, `quota_exceeded`, `stale_handle`, `stale_generation`, `lease_conflict`, `mapping_failure`, `backend_failure`, `bounds_error`, `integrity_failure`, `import_failure`, `overflow`, `lifecycle_violation`, `closed`, and so on.

`Error` is a value carrying an `ErrorCode` and a diagnostic string:

```cpp
struct Error {
  ErrorCode code = ErrorCode::unknown;
  std::string message;
  bool ok() const noexcept;
  explicit operator bool() const noexcept;  // true when not ok
};
```

### `Result<T>`

`Result<T>` is an expected-style type (a discriminated union of a value and an `Error`), chosen because it makes failure explicit and forces the caller to handle it. `Status` is `Result<std::monostate>`, used for operations that succeed with no value. A `Result<void>` specialization exists for functions that return no value.

Key members:

```cpp
bool ok() const noexcept;
explicit operator bool() const noexcept;
ErrorCode error() const noexcept;
const Error& err() const noexcept;
const std::string& message() const noexcept;
T& value() &;
const T& value() const&;
T&& value() &&;
T value_or(T fallback) const;
```

`Result<T>` is default-constructible (yielding a not-ok/empty state), copyable, and movable. It cannot hold references. The convenience helpers `ok(value)`, `ok_status()`, and `ok_void()` construct successes.

---

## Checked arithmetic

The runtime never computes a size, offset, or bounded range with unchecked integer math. [checked_math.hpp](../include/unified_buffer/core/checked_math.hpp) provides:

- `add_overflow(a, b, out)` — false if `a + b` overflows `size_t`.
- `mul_overflow(a, b, out)` — false if `a * b` overflows.
- `align_up(value, alignment)` — returns `std::nullopt` on overflow or for a non-power-of-two alignment.
- `align_down(value, alignment)`.
- `is_aligned(value, alignment)`.
- `range_in_bounds(offset, length, total)` — true only if `[offset, offset+length)` fits in `[0, total)` without overflow.

Examples of where this matters:

- `validate_request` rejects a non-power-of-two or greater-than-4096 alignment, and rejects an `align_up` overflow.
- A `view` uses `range_in_bounds` so `offset + len` cannot wrap and slip past the buffer end.
- `copy_to` and `copy_from` check both `len` against `size` and `range_in_bounds(0, len, committed)`.
- Pool size-class rounding and accounting reuse the same helpers.

---

## Identity vs address

The runtime keeps identity and address conceptually distinct:

- **Identity** (`BufferId`) — stable, process-lifetime within a runtime, the key in the registry, the value carried across boundaries. It is safe to hold for a long time and to pass around.
- **Address** (`NativeAllocation::pointer`, `host_map`) — the actual bytes, which can be moved by migration, pooled (reused for a different buffer), or remapped. It is a *derived* fact about a buffer at a moment in time, and it is only valid while the generation matches and the buffer is alive.

A client uses `BufferHandle::host_data()` or `map()` to *discover* an address, never to *identify* a buffer. The address is borrowed and tied to the handle plus generation; it must not be leaked past the buffer's lifetime.

---

## Single-authority residency (1.0.0)

In 1.0.0 a buffer has exactly one resident backing in exactly one domain at a time. This is what makes migration safe and deterministic:

1. `migrate` reserves capacity in the target domain.
2. It allocates a new backing in the target domain.
3. It copies `min(committed_src, committed_dst)` bytes (via `copy_memory`).
4. It verifies the destination before discarding the source.
5. It swaps the binding: the buffer now points at the new backing, its domain/backend/device fields are updated, its generation is bumped, and it is marked `pool_eligible = false` / `pooled = false`.
6. Only then does it free the old backing and release the old domain accounting.

This ordering guarantees a buffer is never torn between two domains and never loses data if the destination copy or verify fails (the source is untouched until the swap succeeds).

---

## Zeroing policy

`ZeroingPolicy` (in [types.hpp](../include/unified_buffer/core/types.hpp)) gates zeroing behaviour:

| Value | Behaviour at allocation |
| --- | --- |
| `NEVER` | Never zero. |
| `ON_ALLOCATE` | Always zero on allocation. |
| `ON_RELEASE` | Declared to zero on release; not zeroed at allocation. |
| `ON_CROSS_OWNER_REUSE` | Zero only when a pooled slot is reused by a different namespace than its recorded `last_owner`. |
| `ALWAYS` | Always zero (allocation and reuse paths). |

The effective policy is the per-request override if `AllocationRequest::zeroing` is set, otherwise the namespace's `zeroing`. The namespace default is `ON_CROSS_OWNER_REUSE`.

Two notes on what 1.0.0 actually does:

- **Zeroing is applied at allocation time.** `allocate_record` decides `want_zero` from the policy and calls the domain's `zero` on the committed range when true. For `ON_CROSS_OWNER_REUSE`, this happens only when the slot came from the pool AND its recorded owner differs from the requesting namespace.
- **`ON_RELEASE` is a declared policy value, but the release path in 1.0.0 does not perform a zeroing pass.** The release path returns the backing to the pool or frees it; it does not zero. Treat `ON_RELEASE` as reserved semantics: it is not observed today and choosing it behaves like `NEVER` for the allocation-time decision. If you need deterministic zeroing, use `ON_ALLOCATE` or `ALWAYS`.

---

## Invariants

The following invariants hold across the runtime. They are the properties the design guarantees and that the tests check.

### Identity invariant
1. A live buffer has a non-null `BufferId` and is present in the registry exactly once.
2. `BufferId` never changes for the lifetime of a buffer, even across migration and pooling reuse.
3. All handles for a buffer share the same `BufferId`.

### Generation invariant
4. A buffer is created with generation `1`.
5. Any handle, lease, map, or view whose stored generation differs from the current `Record::generation` is rejected (`stale_generation`).
6. Migration increments the generation exactly once and returns a fresh handle for the new generation.
7. A reference to a finalized or never-registered `BufferId` is rejected (`stale_handle`).

### Ownership invariant
8. Every buffer has exactly one `Ownership` at a time, set at creation and not ambiguous.
9. The runtime frees a backing only for non-`BORROWED` ownership; `BORROWED` memory is never freed.
10. `ADOPTED` is treated as `RUNTIME` at finalization (the runtime owns deallocation).
11. No buffer is freed twice: `finalize` runs at most once per control block (guarded by the atomic `finalized` flag).

### Capacity invariant
12. An allocation never exceeds the per-domain capacity cap or the per-namespace quota; violation surfaces as `out_of_capacity` or `quota_exceeded`.
13. Commit only happens after a successful reservation, and unreserve/free is symmetric on every early-return path.
14. Accounting is consistent: committed bytes reported are never negative (all subtraction is clamped).

### Lease invariant
15. `READ` leases may coexist; `WRITE` and `EXCLUSIVE` leases require no other active lease of any kind (`lease_conflict` otherwise).
16. A migration is refused while a `WRITE` or `EXCLUSIVE` lease is held (`lease_conflict`).
17. Releasing the last lease of an `IN_USE` buffer returns it to `ALLOCATED`.

### Mapping invariant
18. `map` of a `DEVICE` buffer fails with `unsupported_capability` (device memory is not CPU-mappable).
19. `map` increments `map_count`; releasing the last map of a `MAPPED` buffer returns it to `ALLOCATED`.
20. A mapped address is borrowed and tied to the handle plus generation; it must not outlive the buffer.

### Pool invariant
21. A pooled slot is only returned to a compatible key (same domain, backend, device, size class, alignment, exportability).
22. Pool size never exceeds `max_objects` or `max_idle_bytes`; an over-limit return is evicted (freed) rather than accumulated.
23. A pool-eligible buffer is only reused when its size kind and policy permit it, and cross-owner reuse is zeroed when required.
24. `pool_trim` frees every idle slot; `pool_trim_to_zero` is used on shutdown.

### Migration invariant
25. Migration requires no `WRITE` or `EXCLUSIVE` lease, a non-`RELEASED` / `INVALID` state, and an enabled target domain.
26. Migration copies the smaller of the source/destination committed sizes; destination is verified before the source is released.
27. After migration, the buffer is `pool_eligible = false` and `pooled = false`; the old backing is freed and old-domain accounting is released.
